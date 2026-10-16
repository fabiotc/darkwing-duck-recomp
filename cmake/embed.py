"""Embed a binary file as a C array: embed.py INPUT SYMBOL OUTPUT.c"""
import sys

data = open(sys.argv[1], "rb").read()
name = sys.argv[2]
rows = [", ".join(f"0x{b:02X}" for b in data[i:i + 16]) for i in range(0, len(data), 16)]
with open(sys.argv[3], "w") as f:
    f.write(f"/* Generated from {sys.argv[1].split('/')[-1]}. Do not edit. */\n")
    f.write(f"const unsigned char {name}[] = {{\n    " + ",\n    ".join(rows) + "\n};\n")
    f.write(f"const unsigned int {name}_len = {len(data)};\n")
