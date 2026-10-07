#include "IFileTransport.h"
#include <unistd.h>

IFileTransport::~IFileTransport() {
    if (fd_ >= 0) ::close(fd_);
}

bool IFileTransport::ObtainMutex() {
    return sem_wait(busSemaphore_) == 0;
}

void IFileTransport::ReleaseMutex() {
    sem_post(busSemaphore_);
}
