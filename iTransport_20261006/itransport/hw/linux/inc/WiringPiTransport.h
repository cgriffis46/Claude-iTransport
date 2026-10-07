#pragma once
#include "IFileTransport.h"

// Shared base for WiringPi-based transports (I2C now; an SPI one
// could follow the same shape later). Now genuinely thin: fd_
// ownership, isBusy()/lastOpFailed(), and close-on-destruction all
// live in IFileTransport, which fits WiringPi naturally since
// wiringPiI2CSetup() itself just hands back a normal POSIX file
// descriptor under the hood — the same "everything is a file" Unix
// idea IFileTransport is built around.
//
// Kept as its own class (rather than collapsing into IFileTransport
// directly) so a future WiringPiSpiTransport has a natural shared
// home for anything specifically WiringPi-related that isn't a plain
// POSIX file concept — e.g. wiringPiSetup()'s own global library init.
class WiringPiTransport : public IFileTransport {
protected:
    using IFileTransport::IFileTransport;

    // writeReg()/readRegs()/checkDevice() are still pure virtual here
    // (inherited from ISensorTransport, unimplemented) — those are
    // the actual hardware calls, left for WiringPiI2cTransport (or a
    // future WiringPiSpiTransport) to supply.
};
