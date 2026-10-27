# NUNI 모델

- `yamnet_emb.tflite`: Google YAMNet을 float32 TFLite로 변환한 모델. 16kHz 오디오 15,600개 표본을 입력받아 521개 소리 점수와 1,024차원 임베딩을 출력한다. 변환 코드는 `cry_model/export_yamnet_tflite.py`에 있다.
- `cry_personal_ctx.json`: 시간과 이전 울음·수유 간격을 입력으로 수유·기저귀·안아주기 안내 순위를 계산하는 맥락 모델.

YAMNet 원본: https://www.kaggle.com/models/google/yamnet/tensorFlow2/yamnet/1

YAMNet 코드: https://github.com/tensorflow/models/tree/master/research/audioset/yamnet

YAMNet 라이선스: Apache License 2.0 (`LICENSE-YAMNET.txt`). 원본 모델을 NUNI의 고정 입력 길이와 두 출력 형식으로 변환했다.
