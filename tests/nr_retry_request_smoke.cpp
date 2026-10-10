// Host check of dlssnr/DlssNr_RetryRequest.h: one retry per request, however many are piled up, per consumer.
// cl /std:c++20 /EHsc /W4 tests/nr_retry_request_smoke.cpp
#include "../OptiScaler/dlssnr/DlssNr_RetryRequest.h"

#include <cstdio>

int main()
{
    DlssNr::RetryRequest request;
    unsigned int handled = 0;
    unsigned int other = 0;
    int fails = 0;
    const auto check = [&](bool ok, const char* what) {
        if (!ok)
        {
            std::printf("FAIL: %s\n", what);
            ++fails;
        }
    };

    check(!request.Consume(handled), "nothing requested, nothing consumed");
    request.Request();
    check(request.Consume(handled), "a request is consumed");
    check(!request.Consume(handled), "and only once");
    request.Request();
    request.Request();
    request.Request();
    check(request.Consume(handled), "several requests make one retry");
    check(!request.Consume(handled), "and are then spent");
    check(request.Consume(other), "another consumer has its own record");
    check(!request.Consume(other), "which is also spent");

    // What a retry does about the device: never call the old one once the game has made a new one.
    using DlssNr::RetryStep;
    using DlssNr::DecideRetry;
    check(DecideRetry(true, true, false, true, false) == RetryStep::Release, "same device: drain and release");
    check(DecideRetry(true, true, false, true, true) == RetryStep::Abandon, "new device: abandon, call nothing");
    check(DecideRetry(true, true, false, false, true) == RetryStep::Release, "no device kept: nothing to call either way");
    check(DecideRetry(false, true, false, true, true) == RetryStep::None, "no request");
    check(DecideRetry(true, false, false, true, true) == RetryStep::None, "nothing failed");
    check(DecideRetry(true, true, true, true, false) == RetryStep::None, "a failure a retry cannot fix");

    if (fails == 0)
        std::puts("PASS: nr_retry_request_smoke");
    return fails != 0;
}
