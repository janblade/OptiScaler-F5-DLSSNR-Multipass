#pragma once
// Producer ABI metadata already published by DLSS5 Feeder, including its x64 Host.
// A completed external image has no native-game Present on the private NGX device.
// Validate the full tuple; ordinary game DLSS calls retain their placement mode.
namespace DlssNrFrameProducer {
template<class Read> bool IsFeederFrame(Read&& read) {
    unsigned version=0,provider=0,host=0,cadence=0;
    return read("DFC.Feeder.ContractVersion",&version) &&
           read("DFC.Feeder.ProviderId",&provider) &&
           read("DFC.Feeder.HostMode",&host) &&
           read("DFC.Feeder.EvaluateCadence",&cadence) &&
           version==1u && provider==0x444c3546u && host<=1u && cadence==1u;
}
}
