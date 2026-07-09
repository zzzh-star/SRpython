from .grasp_state_machine import GraspState


class OmegaFeedbackPolicy:
    def __init__(self, feedback_config: dict, state_config: dict):
        self.gain = float(feedback_config["gain"])
        self.over_gain = float(feedback_config["over_gain"])
        self.force_limit = float(feedback_config["force_limit"])
        self.deadband = float(feedback_config.get("deadband", 0.05))
        stable_low = float(state_config["F_stable_low"])
        stable_high = float(state_config["F_stable_high"])
        self.f_ref = 0.5 * (stable_low + stable_high)
        self.f_stable_high = stable_high

    def compute(self, fn: float, state: GraspState) -> float:
        if state == GraspState.NONE:
            feedback = 0.0
        elif state == GraspState.CONTACT:
            feedback = -0.35 * self.gain * max(0.0, fn - self.f_ref)
        else:
            feedback = -self.gain * (fn - self.f_ref)
            if state == GraspState.OVER:
                feedback += -self.over_gain * max(0.0, fn - self.f_stable_high)

        if abs(feedback) < self.deadband:
            feedback = 0.0

        return max(-self.force_limit, min(self.force_limit, feedback))
