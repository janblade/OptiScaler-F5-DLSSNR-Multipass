#include "../OptiScaler/shaders/dlssnr/DlssNr_ExternalSubmission.h"
#include <cstdio>
#include <cstdlib>

int main()
{
    using Tracker = DlssNrExternalSubmission<int>;
    Tracker tracker;
    int a = 0, b = 0;
    unsigned checks = 0;
    auto check = [&](bool ok) { ++checks; if (!ok) { printf("FAIL check %u\n", checks); std::exit(1); } };
    auto first = tracker.Record(&a);
    check(first && first->status == Tracker::Status::Recording);
    check(tracker.Record(&a) == first); // another Evaluate is not a submit
    check(!tracker.Submitted(&b, 7)); // another list is not evidence
    check(first->status == Tracker::Status::Recording);
    tracker.Reset(&a);
    check(first->status == Tracker::Status::Discarded);
    auto next = tracker.Record(&a);
    check(next != first && next->serial > first->serial);
    auto extraPass = tracker.Record(&b);
    check(tracker.Submitted(&a, 42));
    check(next->status == Tracker::Status::Submitted && next->completion == 42);
    check(extraPass->status == Tracker::Status::Recording);
    tracker.Reset(&a); // Reset after submit cannot revoke a submitted recording
    check(next->status == Tracker::Status::Submitted);
    check(!tracker.Submitted(&a, 99)); // duplicate notification is not new progress
    tracker.Clear();
    check(extraPass->status == Tracker::Status::Discarded);
    check(next->completion == 42);
    check(!tracker.Record(nullptr));
    printf("NR exact-recording submission tickets: %u checks PASS\n", checks);
    return 0;
}
