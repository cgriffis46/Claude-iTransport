#include "SensorBase.h"

void SensorBase::main(uint32_t nowMs) {
    switch (state_) {

    case State::Calibrate:
        if (!hasCalibrationData()) {
            state_ = State::Configure;   // nothing to fetch — most sensors take this path
            break;
        }
        if (!calibRequested_) {
            if (loadCalibration()) {
                calibRequested_ = true;
            } else {
                sleep(1); // transport busy with something else — wait a beat and retry
            }
        } else if (!transport_.isBusy()) {
            calibRequested_ = false;
            if (!transport_.lastOpFailed()) {
                onCalibrationRead();
                state_ = State::Configure;
            }
            // else: read failed — retry next call (calibRequested_ already false)
        } else {
            sleep(1); // still waiting on the calibration read to land
        }
        break;

    case State::Configure:
        if (!configureRequested_) {
            if (configureDevice()) {
                configureRequested_ = true;
            } else {
                sleep(1); // transport busy with something else — wait a beat and retry
            }
        } else if (!transport_.isBusy()) {
            configureRequested_ = false;
            if (!transport_.lastOpFailed()) {
                state_ = State::Idle;
            }
            // else: write failed — retry next call (configureRequested_ already false)
        } else {
            sleep(1); // still waiting on the configuration write to land
        }
        break;

    case State::Idle:
        if (nowMs - tMark_ >= pollIntervalMs()) {
            if (transport_.writeReg(triggerRegister(), triggerValue())) {
                tMark_ = nowMs;
                state_ = State::Trigger;
            } else {
                sleep(1); // transport busy with something else — wait a beat and retry
            }
        } else {
            sleep(1); // waiting for the poll interval to elapse
        }
        break;

    case State::Trigger:
        if (!transport_.isBusy()) {
            if (transport_.lastOpFailed()) {
                state_ = State::Error;
            } else {
                tMark_ = nowMs;
                state_ = State::WaitConv;
            }
        } else if (nowMs - tMark_ > i2cTimeoutMs()) {
            state_ = State::Error;
        } else {
            sleep(1); // still waiting on the trigger write to land
        }
        break;

    case State::WaitConv:
        // The chip is converting on its own silicon right now.
        // sleep() is how we wait for that — either a no-op (bare
        // superloop: main() returns, gets called again) or a real
        // yield (RTOS: this task steps aside while the chip works).
        if (nowMs - tMark_ < conversionTimeMs()) {
            sleep(1); // still converting per the timer
            break;
        }
        if (hasMeasuringFlag()) {
            if (!measuringCheckPending_) {
                if (checkMeasuring()) {
                    measuringCheckPending_ = true;
                } else {
                    sleep(1); // transport busy with something else — retry next call
                }
                break;
            }
            if (transport_.isBusy()) {
                sleep(1); // still waiting on the measuring-status read to land
                break;
            }
            measuringCheckPending_ = false;
            if (!transport_.lastOpFailed() && stillMeasuring()) {
                sleep(1); // confirmed still measuring — check again next call
                break;
            }
            // else: confirmed done (or the status read itself failed —
            // proceed rather than get stuck forever) — fall through
        }
        if (dataLength() <= kMaxDataLen &&
            transport_.readRegs(dataRegister(), raw_, dataLength())) {
            state_ = State::Read;
        } else {
            sleep(1); // transport busy with something else — wait a beat and retry
        }
        break;

    case State::Read:
        if (!transport_.isBusy()) {
            if (transport_.lastOpFailed()) {
                if (++retry_ > maxRetry()) {
                    retry_ = 0;
                    state_ = State::Error;
                }
                // else: drop back to WaitConv-style retry by re-issuing
                // the read next call (state_ stays Read, isBusy() will
                // be false, and we'll call readRegs again below).
                else if (!transport_.readRegs(dataRegister(), raw_, dataLength())) {
                    sleep(1); // transport busy elsewhere — wait a beat and retry
                }
            } else {
                retry_ = 0;
                state_ = State::Process;
            }
        } else {
            sleep(1); // still waiting on the data read to land
        }
        break;

    case State::Process:
        decode(raw_);
        newReadingReady_ = true;
        tMark_ = nowMs;
        state_ = State::NotifyData;
        break;

    case State::NotifyData:
        if (newDataCallback_) {
            newDataCallback_(*this, callbackContext_);
        }
        state_ = State::Idle;
        break;

    case State::Error:
        if (nowMs - tMark_ >= backoffMs()) {
            tMark_ = nowMs;
            state_ = State::Idle;
        } else {
            sleep(1); // backing off before retrying
        }
        break;
    }
}
