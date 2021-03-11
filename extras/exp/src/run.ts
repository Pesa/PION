import pDefer, { DeferredPromise } from "p-defer";

import { Authenticator } from "./authenticator";
import { AppState, Device } from "./device";
import { Dumpcap } from "./dumpcap";
import { env } from "./env";
import { WifiStation } from "./wifi-station";

function delay(duration: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, duration));
}

/** Perform a single run of the experiment. */
export class Run {
  private l!: Console;
  private defer!: DeferredPromise<Run.Result>;
  private device?: Device;
  private authenticator?: Authenticator;
  private directWifi?: WifiStation;
  private directDump?: Dumpcap;
  private infraDump?: Dumpcap;

  public run({
    logger = process.stderr,
  }: Run.Options = {}): Promise<Run.Result> {
    this.l = new console.Console(logger);
    this.defer = pDefer<Run.Result>();
    this.defer.promise.finally(this.cleanup);
    this.initDevice();
    return this.defer.promise;
  }

  private initDevice(): void {
    this.device = new Device(env.deviceSerial);
    this.device.on("state", this.handleDeviceState);
    this.device.on("line", (line) => this.l.debug("device", line));
    // eslint-disable-next-line promise/prefer-await-to-then
    void this.device.once("error").then((err) => this.defer.reject(err));
  }

  private handleDeviceState = async (state: AppState) => {
    switch (state) {
      case AppState.WaitDirectConnect:
        await this.directConnect();
        break;
      case AppState.WaitPake:
        this.startAuthenticator();
        break;
      case AppState.WaitDirectDisconnect:
        await this.directDisconnect();
        break;
      case AppState.WaitInfraConnect:
        this.startInfraDump();
        break;
      case AppState.Final:
        await this.finish();
        break;
    }
  };

  private async directConnect(): Promise<void> {
    await delay(1000);
    if (this.device!.program.includes("direct-wifi")) {
      this.directDump = new Dumpcap(env.directWifiNetif);
      await delay(500);

      this.directWifi = new WifiStation();
      await this.directWifi.connect({
        ctrl: env.directWifiWpaCtrl,
        netif: env.directWifiNetif,
        ssid: env.directWifiSsid,
        passphrase: env.directWifiPassphrase,
        localIp: `${env.directWifiAuthIp}/${env.directWifiSubnet}`,
      });
    } else if (this.device!.program.includes("direct-ble")) {
      throw new Error("direct=ble not implemented");
    } else {
      throw new Error("unknown direct connection method");
    }
  }

  private startAuthenticator(): void {
    this.authenticator = new Authenticator({
      deviceIp: env.directWifiDeviceIp,
      devicePort: 6363,
      mtu: undefined,
      keychain: env.keychain,
      caProfile: env.caProfile,
      deviceName: `${env.networkPrefix}/d${Date.now()}`,
      networkCredential: `${env.infraWifiSsid}\n${env.infraWifiPassphrase}\n${env.infraWifiGatewayIp}`,
      pakePassword: this.device!.password,
    });
    this.authenticator.on("line", (line) => this.l.debug("authenticator", line));
    // eslint-disable-next-line promise/prefer-await-to-then
    void this.authenticator.once("error").then((err) => this.defer.reject(err));
  }

  private async directDisconnect(): Promise<void> {
    await this.directWifi?.disconnect();
  }

  private startInfraDump(): void {
    this.infraDump = new Dumpcap(env.infraWifiNetif);
  }

  private cleanup = async () => {
    this.device?.close();
    this.authenticator?.close();
    await this.directWifi?.disconnect();
    await this.directDump?.close();
    await this.infraDump?.close();
  };

  private async finish(): Promise<void> {
    await this.cleanup();
    this.defer.resolve({
      program: this.device!.program,
      device: this.device!.result,
      authenticator: this.authenticator?.result,
      directDump: this.directDump?.pcap?.toString("base64"),
      infraDump: this.infraDump?.pcap?.toString("base64"),
    });
  }
}

export namespace Run {
  export interface Options {
    logger?: NodeJS.WritableStream;
  }

  export interface Result {
    program: string[];
    device: Device.Result;
    authenticator?: Authenticator.Result;
    directDump?: string;
    infraDump?: string;
  }
}
