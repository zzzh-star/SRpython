from enum import Enum


class GraspState(Enum):
    NONE = "NONE"
    CONTACT = "CONTACT"
    STABLE = "STABLE"
    OVER = "OVER"


class GraspStateMachine:
    def __init__(self, config: dict):
        self.f_contact = float(config["F_contact"])
        self.f_stable_low = float(config["F_stable_low"])
        self.f_stable_high = float(config["F_stable_high"])
        self.f_over_enter = float(config.get("F_over_enter", self.f_stable_high))
        self.f_over_exit = float(config.get("F_over_exit", self.f_stable_high))
        self.confirm_frames = int(config.get("state_confirm_frames", 5))
        self.min_hold_s = float(config.get("state_min_hold_ms", 100)) / 1000.0
        self.state = GraspState.NONE
        self._candidate = self.state
        self._candidate_count = 0
        self._last_change_time = 0.0

    def update(self, fn: float, now_s: float):
        target = self._classify(fn)
        changed = False

        if target != self._candidate:
            self._candidate = target
            self._candidate_count = 1
        else:
            self._candidate_count += 1

        can_change = now_s - self._last_change_time >= self.min_hold_s
        if target != self.state and self._candidate_count >= self.confirm_frames and can_change:
            self.state = target
            self._last_change_time = now_s
            changed = True

        return self.state, changed

    def _classify(self, fn: float) -> GraspState:
        if self.state == GraspState.OVER and fn >= self.f_over_exit:
            return GraspState.OVER
        if fn >= self.f_over_enter:
            return GraspState.OVER
        if self.f_stable_low <= fn <= self.f_stable_high:
            return GraspState.STABLE
        if fn >= self.f_contact:
            return GraspState.CONTACT
        return GraspState.NONE
