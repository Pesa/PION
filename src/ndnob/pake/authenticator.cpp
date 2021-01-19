#include "authenticator.hpp"

namespace ndnob {
namespace pake {

class Authenticator::GotoState
{
public:
  explicit GotoState(Authenticator* authenticator)
    : m_authenticator(authenticator)
  {}

  bool operator()(State state)
  {
    m_authenticator->m_state = state;
    m_set = true;
    return true;
  }

  ~GotoState()
  {
    if (!m_set) {
      m_authenticator->m_state = State::Failure;
    }
  }

private:
  Authenticator* m_authenticator;
  bool m_set = false;
};

class Authenticator::PakeRequest : public packet_struct::PakeRequest
{
public:
  ndnph::Interest::Parameterized toInterest(ndnph::Region& region, EncryptSession& session) const
  {
    ndnph::Encoder encoder(region);
    encoder.prependTlv(TT::Spake2T, ndnph::tlv::Value(spake2T, sizeof(spake2T)));
    encoder.trim();

    ndnph::Interest interest = region.create<ndnph::Interest>();
    if (!encoder || !interest) {
      return ndnph::Interest::Parameterized();
    }
    interest.setName(session.makeName(region, getPakeComponent()));
    return interest.parameterize(ndnph::tlv::Value(encoder));
  }
};

class Authenticator::PakeResponse : public packet_struct::PakeResponse
{
public:
  bool fromData(ndnph::Region&, const ndnph::Data& data)
  {
    return ndnph::EvDecoder::decodeValue(
      data.getContent().makeDecoder(),
      ndnph::EvDecoder::def<TT::Spake2S>([this](const ndnph::Decoder::Tlv& d) {
        if (d.length == sizeof(spake2S)) {
          std::copy_n(d.value, d.length, spake2S);
          return true;
        }
        return false;
      }),
      ndnph::EvDecoder::def<TT::Spake2Fkcb>([this](const ndnph::Decoder::Tlv& d) {
        if (d.length == sizeof(spake2Fkcb)) {
          std::copy_n(d.value, d.length, spake2Fkcb);
          return true;
        }
        return false;
      }));
  }
};

class Authenticator::ConfirmRequest : public packet_struct::ConfirmRequest
{
public:
  ndnph::Interest::Parameterized toInterest(ndnph::Region& region, EncryptSession& session) const
  {
    auto encrypted = session.encrypt(
      region, [this](ndnph::Encoder& encoder) { encoder.prependTlv(TT::Nc, nc); },
      [this](ndnph::Encoder& encoder) { encoder.prependTlv(TT::CaProfileName, caProfileName); },
      [this](ndnph::Encoder& encoder) {
        encoder.prependTlv(TT::AuthenticatorCertName, authenticatorCertName);
      },
      [this](ndnph::Encoder& encoder) { encoder.prependTlv(TT::DeviceName, deviceName); },
      ndnph::convention::Timestamp::create(region, ndnph::convention::TimeValue()));

    ndnph::Encoder outer(region);
    outer.prepend(
      [this](ndnph::Encoder& encoder) {
        encoder.prependTlv(TT::Spake2Fkca, ndnph::tlv::Value(spake2Fkca, sizeof(spake2Fkca)));
      },
      encrypted);
    outer.trim();

    ndnph::Interest interest = region.create<ndnph::Interest>();
    if (!encrypted || !outer || !interest) {
      return ndnph::Interest::Parameterized();
    }
    interest.setName(session.makeName(region, getConfirmComponent()));
    return interest.parameterize(ndnph::tlv::Value(outer));
  }
};

class Authenticator::ConfirmResponse : public packet_struct::ConfirmResponse
{
public:
  bool fromData(ndnph::Region& region, const ndnph::Data& data, EncryptSession& session)
  {
    Encrypted encrypted;
    bool ok = ndnph::EvDecoder::decodeValue(
      data.getContent().makeDecoder(), ndnph::EvDecoder::def<TT::InitializationVector>(&encrypted),
      ndnph::EvDecoder::def<TT::AuthenticationTag>(&encrypted),
      ndnph::EvDecoder::def<TT::EncryptedPayload>(&encrypted));
    if (!ok) {
      return false;
    }

    auto inner = session.decrypt(region, encrypted);
    return !!inner && ndnph::EvDecoder::decodeValue(
                        inner.makeDecoder(),
                        ndnph::EvDecoder::def<TT::TReq>([&](const ndnph::Decoder::Tlv& d) {
                          tempCertReq = region.create<ndnph::Data>();
                          return !!tempCertReq && d.vd().decode(tempCertReq) &&
                                 tPub.import(region, tempCertReq);
                        }));
  }

public:
  ndnph::EcPublicKey tPub;
};

class Authenticator::CredentialRequest : public packet_struct::CredentialRequest
{
public:
  ndnph::Interest::Parameterized toInterest(ndnph::Region& region, EncryptSession& session) const
  {
    auto encrypted = session.encrypt(region, [this](ndnph::Encoder& encoder) {
      encoder.prependTlv(TT::IssuedCertName, tempCertName);
    });

    ndnph::Interest interest = region.create<ndnph::Interest>();
    if (!encrypted || !interest) {
      return ndnph::Interest::Parameterized();
    }
    interest.setName(session.makeName(region, getCredentialComponent()));
    return interest.parameterize(encrypted);
  }
};

Authenticator::Authenticator(const Options& opts)
  : PacketHandler(opts.face, 192)
  , m_caProfile(opts.caProfile)
  , m_cert(opts.cert)
  , m_pvt(opts.pvt)
  , m_nc(opts.nc)
  , m_deviceName(opts.deviceName)
  , m_pending(this)
  , m_region(4096)
{}

void
Authenticator::end()
{
  m_session.end();
  m_spake2.reset();
  m_state = State::Idle;
  m_region.reset();
}

bool
Authenticator::begin(ndnph::tlv::Value password)
{
  end();

  if (!m_session.begin(m_region)) {
    return false;
  }

  mbed::Object<mbedtls_entropy_context, mbedtls_entropy_init, mbedtls_entropy_free> entropy;
  m_spake2.reset(new spake2::Context<>(spake2::Role::Alice, entropy));
  if (!m_spake2->start(password.begin(), password.size())) {
    return false;
  }

  m_state = State::SendPakeRequest;
  return true;
}

void
Authenticator::loop()
{
  switch (m_state) {
    case State::SendPakeRequest: {
      sendPakeRequest();
      break;
    }
    case State::SendCredentialRequest: {
      sendCredentialRequest();
      break;
    }
    case State::WaitPakeResponse:
    case State::WaitConfirmResponse:
    case State::WaitCredentialResponse: {
      if (m_pending.expired()) {
        m_state = State::Failure;
      }
      break;
    }
    default:
      break;
  }
}

bool
Authenticator::processData(ndnph::Data data)
{
  if (!m_pending.matchPitToken()) {
    return false;
  }
  switch (m_state) {
    case State::WaitPakeResponse: {
      return handlePakeResponse(data);
    }
    case State::WaitConfirmResponse: {
      return handleConfirmResponse(data);
    }
    case State::WaitCredentialResponse: {
      m_state = State::Success;
      return true;
    }
    default:
      break;
  }
  return false;
}

void
Authenticator::sendPakeRequest()
{
  ndnph::StaticRegion<2048> region;
  GotoState gotoState(this);
  PakeRequest req;
  m_spake2->generateFirstMessage(req.spake2T, sizeof(req.spake2T)) &&
    m_pending.send(req.toInterest(region, m_session)) && gotoState(State::WaitPakeResponse);
}

bool
Authenticator::handlePakeResponse(ndnph::Data data)
{
  ndnph::StaticRegion<2048> region;
  PakeResponse res;
  if (!res.fromData(region, data)) {
    return false;
  }

  GotoState gotoState(this);
  ConfirmRequest req;
  bool ok = m_spake2->processFirstMessage(res.spake2S, sizeof(res.spake2S)) &&
            m_spake2->generateSecondMessage(req.spake2Fkca, sizeof(req.spake2Fkca)) &&
            m_spake2->processSecondMessage(res.spake2Fkcb, sizeof(res.spake2Fkcb)) &&
            m_session.importKey(m_spake2->getSharedKey());
  m_spake2.reset();
  if (!ok) {
    return true;
  }

  req.nc = m_nc;
  req.caProfileName = m_caProfile.getFullName(region);
  req.authenticatorCertName = m_cert.getFullName(region);
  req.deviceName = m_deviceName;
  // req.timestamp is ignored; current timestamp will be used
  m_pending.send(req.toInterest(region, m_session)) && gotoState(State::WaitConfirmResponse);
  return true;
}

bool
Authenticator::handleConfirmResponse(ndnph::Data data)
{
  ndnph::StaticRegion<2048> region;
  ConfirmResponse res;
  if (!res.fromData(region, data, m_session)) {
    return false;
  }

  GotoState gotoState(this);
  auto subjectName = computeTempSubjectName(region, m_cert.getName(), m_deviceName);
  if (ndnph::certificate::toSubjectName(region, res.tempCertReq.getName()) != subjectName) {
    return true;
  }

  time_t now = time(nullptr);
  ndnph::ValidityPeriod validity(now, now + TempCertValidity::value);
  ndnph::Encoder encoder(m_region);
  encoder.prepend(res.tPub.buildCertificate(region, subjectName, validity, m_pvt));
  if (!encoder) {
    encoder.discard();
    return true;
  }
  encoder.trim();

  m_issued = m_region.create<ndnph::Data>();
  return !!m_issued && ndnph::tlv::Value(encoder).makeDecoder().decode(m_issued) &&
         gotoState(State::SendCredentialRequest);
}

void
Authenticator::sendCredentialRequest()
{
  ndnph::StaticRegion<2048> region;
  GotoState gotoState(this);
  CredentialRequest req;
  req.tempCertName = m_issued.getFullName(region);
  !!req.tempCertName && m_pending.send(req.toInterest(region, m_session)) &&
    gotoState(State::WaitCredentialResponse);
}

bool
Authenticator::processInterest(ndnph::Interest interest)
{
  if (interest.match(m_caProfile)) {
    return reply(m_caProfile);
  }
  if (interest.match(m_cert)) {
    return reply(m_cert);
  }
  if (!!m_issued && interest.match(m_issued)) {
    return reply(m_issued);
  }
  return false;
}

} // namespace pake
} // namespace ndnob