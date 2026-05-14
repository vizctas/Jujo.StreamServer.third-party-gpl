#pragma once

namespace display_device {
  enum class DisplayRecoveryBehavior {
    Automatic,
    Skip,
  };

  class DisplayRecoveryBehaviorGuard {
   public:
    explicit DisplayRecoveryBehaviorGuard(DisplayRecoveryBehavior behavior);
    DisplayRecoveryBehaviorGuard(const DisplayRecoveryBehaviorGuard &) = delete;
    DisplayRecoveryBehaviorGuard &operator=(const DisplayRecoveryBehaviorGuard &) = delete;
    DisplayRecoveryBehaviorGuard(DisplayRecoveryBehaviorGuard &&) = delete;
    DisplayRecoveryBehaviorGuard &operator=(DisplayRecoveryBehaviorGuard &&) = delete;
    ~DisplayRecoveryBehaviorGuard();

   private:
    DisplayRecoveryBehavior m_previous;
  };

  namespace detail {
    DisplayRecoveryBehavior current_display_recovery_behavior();
    void set_display_recovery_behavior(DisplayRecoveryBehavior behavior);
  }  // namespace detail
}  // namespace display_device
