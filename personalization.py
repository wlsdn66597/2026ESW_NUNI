"""관측용 호흡·움직임 통계. 고유한 유효 관측만 집계하며 경보 임계값을 바꾸지 않는다."""
import math


class Personalizer:
    def __init__(self, alpha=0.05):
        self.alpha = alpha
        self.br_mean = None
        self.br_var = 0.0
        self.move_mean = None
        self.n = 0

    def update(self, breathing_rate, movement):
        self.n += 1
        if breathing_rate and breathing_rate > 0:          # 무호흡(0)은 baseline에서 제외
            if self.br_mean is None:
                self.br_mean = float(breathing_rate)
            else:
                d = breathing_rate - self.br_mean
                self.br_mean += self.alpha * d
                self.br_var = (1 - self.alpha) * (self.br_var + self.alpha * d * d)
        if movement is not None:
            self.move_mean = float(movement) if self.move_mean is None \
                else self.move_mean + self.alpha * (movement - self.move_mean)

    def normal_bpm_range(self, k=2.0):
        """개인 호흡 정상 범위 (mean ± k·sd)."""
        if self.br_mean is None:
            return None
        sd = math.sqrt(max(self.br_var, 0.0))
        return (round(self.br_mean - k * sd, 1), round(self.br_mean + k * sd, 1))

    def restless_threshold(self, factor=2.0, floor=0.4):
        """개인 움직임 baseline 기반 뒤척임 임계값."""
        base = self.move_mean if self.move_mean is not None else 0.1
        return round(max(floor, base * factor + 0.2), 2)

    def summary(self):
        return {
            "samples": self.n,
            "mode": "observation_only",
            "sample_definition": "unique_valid_quiet_breath",
            "bpm_mean": None if self.br_mean is None else round(self.br_mean, 1),
            "bpm_normal_range": self.normal_bpm_range(),
            "restless_threshold": self.restless_threshold(),
        }
