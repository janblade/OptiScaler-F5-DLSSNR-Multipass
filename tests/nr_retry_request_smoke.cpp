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

    if (fails == 0)
        std::puts("PASS: nr_retry_request_smoke");
    return fails != 0;
}
