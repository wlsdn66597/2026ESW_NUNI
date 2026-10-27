"""YAMNet(tflite) 직결 울음 감지 — 커스텀 헤드 학습 없이 사용.

cry_model/infer.py 의 CryModel은 커스텀 학습 헤드(cry_head.keras)가 필요한데
아직 학습 데이터가 없다. 대신 YAMNet 자체에 이미 있는 클래스
"Baby cry, infant cry"(idx 20)와 "Crying, sobbing"(idx 19) 점수를 직접 써서
울음 유무만 판정한다. 울음 **이유** 분류는 하지 않는다(cry_context.py로 우회 —
프로젝트 정직성 원칙: 이유 분류는 실험적, 대신 환경/시간 맥락 상관 사용).

모델: TF Hub YAMNet tflite (16kHz mono, 15600샘플=0.975초 고정 입력 → 521클래스 점수).
런타임: ai-edge-litert (tflite_runtime 후속, Python 3.13/aarch64 지원).
"""
import csv
import os

import numpy as np
from ai_edge_litert.interpreter import Interpreter

_HERE = os.path.dirname(__file__)
MODEL_PATH = os.path.join(_HERE, "artifacts", "yamnet.tflite")
CLASS_MAP_PATH = os.path.join(_HERE, "artifacts", "yamnet_class_map.csv")
WINDOW = 15600           # YAMNet 고정 입력 길이 (0.975초 @ 16kHz)
SR = 16000

CRY_LABELS = ("Baby cry, infant cry", "Crying, sobbing")


def resample_linear(x, orig_sr, target_sr=SR):
    """선형보간 리샘플. USB 마이크가 16kHz 직캡처를 지원하지 않을 때
    (하드웨어 기본 48kHz 등) 마이크 자체 레이트로 받은 뒤 이걸로 낮춘다.
    음질용이 아니라 분류기 입력 정합 목적이라 이 정도 품질로 충분하다."""
    if orig_sr == target_sr:
        return np.asarray(x, dtype=np.float32)
    x = np.asarray(x, dtype=np.float32)
    n_out = int(round(len(x) * target_sr / orig_sr))
    t_in = np.linspace(0, 1, len(x), endpoint=False)
    t_out = np.linspace(0, 1, n_out, endpoint=False)
    return np.interp(t_out, t_in, x).astype(np.float32)


def _load_class_indices(names):
    with open(CLASS_MAP_PATH, encoding="utf-8") as f:
        rows = list(csv.reader(f))
    idx = {}
    for row in rows[1:]:                       # 헤더 스킵: index,mid,display_name
        i, _, name = row[0], row[1], row[2]
        if name in names:
            idx[name] = int(i)
    return idx


class YamnetCry:
    def __init__(self):
        self._it = Interpreter(model_path=MODEL_PATH)
        self._it.allocate_tensors()
        self._in_idx = self._it.get_input_details()[0]["index"]
        self._out_idx = self._it.get_output_details()[0]["index"]
        self._cry_idx = _load_class_indices(CRY_LABELS)

    @classmethod
    def load(cls):
        return cls()

    def predict(self, wav16k):
        """wav16k: float32 1-D array(-1~1), 16kHz mono. 길이가 다르면
        WINDOW로 잘라내거나 0-padding한다. → (cry_score 0~1, dominant_label)."""
        x = np.asarray(wav16k, dtype=np.float32).reshape(-1)
        if len(x) < WINDOW:
            x = np.pad(x, (0, WINDOW - len(x)))
        else:
            x = x[-WINDOW:]
        self._it.set_tensor(self._in_idx, x)
        self._it.invoke()
        scores = self._it.get_tensor(self._out_idx)[0]     # (521,)
        cry_scores = {name: float(scores[i]) for name, i in self._cry_idx.items()}
        label = max(cry_scores, key=cry_scores.get)
        return cry_scores[label], label
