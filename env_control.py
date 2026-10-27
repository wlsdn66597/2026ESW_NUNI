"""선제 환경 제어(권장 동작) 로직.

환경 센서(+수면 상태)를 보고 자율적으로 권장 동작을 생성한다. 실제 구동은
스마트플러그/IR 허브로 확장(2차). 여기서는 동작 권고를 산출·발행한다.
울음 이유 분류에 의존하지 않는다.
"""
CO2_HIGH = 1000
HUM_LOW = 35
HUM_HIGH = 60
TEMP_HIGH = 26
TEMP_LOW = 18
SLEEP_LUX = 40


def recommend(env, sleep_state=None):
    """→ [(action, reason), ...]"""
    if not env:
        return []
    actions = []
    co2 = env.get("co2", 0)
    hum = env.get("humidity", 50)
    temp = env.get("temp", 22)
    lux = env.get("lux", 0)

    if co2 > CO2_HIGH:
        actions.append(("ventilate", "CO₂ 높음 → 외기 환기 확인"))
    if hum < HUM_LOW:
        actions.append(("humidify", "습도 낮음 → 가습 고려"))
    elif hum > HUM_HIGH:
        actions.append(("dehumidify", "습도 높음 → 제습/환기"))
    if temp > TEMP_HIGH:
        actions.append(("cool", "온도 높음 → 냉방·통풍"))
    elif temp < TEMP_LOW:
        actions.append(("heat", "온도 낮음 → 난방"))
    if sleep_state in ("calm_sleep", "restless") and lux > SLEEP_LUX:
        actions.append(("dim_light", "수면 중 밝음 → 조명 낮춤"))
    return actions
