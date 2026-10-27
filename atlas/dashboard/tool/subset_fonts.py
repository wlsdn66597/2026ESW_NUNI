"""대시보드 글꼴 줄이기(fontTools 필요).

  고딕(Noto Sans KR, 가변) : 라틴 · 문장 부호 · 기호 · 한글 음절 전체(11,172자) — 서비스가 보내는 어떤 글자도 보이게
  명조(Noto Serif KR, 가변): 라틴 · 문장 부호 · 기호 + lib/ 아래 Dart 코드에 적힌 한글만 — 명조는 상태 문장·질문 같은
                            고정 문구에만 쓴다(문구를 바꾸면 이 스크립트를 다시 돌린다)
가변 글꼴의 굵기 축(wght)과 이름 표(라이선스 정보 포함)는 그대로 둔다.

사용법(atlas/dashboard에서): python tool/subset_fonts.py [--src C:/Windows/Fonts]
  원본: Noto Sans KR / Noto Serif KR 가변 글꼴(SIL OFL 1.1, Google Fonts)
"""
import argparse
import glob
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BASE = "U+0020-007E,U+00A0-00FF,U+2000-206F,U+2070-209F,U+2100-214F,U+2190-21FF,U+2200-22FF,U+2460-24FF," \
       "U+25A0-25FF,U+3000-303F,U+3130-318F,U+FF00-FFEF"


def hangul_in_sources():
    chars = set()
    for path in glob.glob(os.path.join(ROOT, "lib", "**", "*.dart"), recursive=True):
        chars |= set(re.findall(r"[\uac00-\ud7a3]", open(path, encoding="utf-8").read()))
    return sorted(chars)


def subset(src, dst, unicodes):
    cmd = [sys.executable, "-m", "fontTools.subset", src, f"--unicodes={unicodes}", "--layout-features=*",
           "--name-IDs=*", "--name-legacy", "--name-languages=*", "--notdef-outline", f"--output-file={dst}"]
    subprocess.run(cmd, check=True)
    print(f"{os.path.basename(dst)}: {os.path.getsize(dst) / 1e6:.1f} MB")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default="C:/Windows/Fonts", help="NotoSansKR-VF.ttf · NotoSerifKR-VF.ttf가 있는 폴더")
    a = ap.parse_args()
    out = os.path.join(ROOT, "assets", "fonts")
    os.makedirs(out, exist_ok=True)
    subset(os.path.join(a.src, "NotoSansKR-VF.ttf"), os.path.join(out, "NotoSansKR-VF.ttf"), BASE + ",U+AC00-D7A3")
    han = hangul_in_sources()
    serif = BASE + "," + ",".join(f"U+{ord(c):04X}" for c in han)
    subset(os.path.join(a.src, "NotoSerifKR-VF.ttf"), os.path.join(out, "NotoSerifKR-VF.ttf"), serif)
    print(f"명조에 넣은 한글 {len(han)}자")


if __name__ == "__main__":
    main()
