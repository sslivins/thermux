#!/usr/bin/env python3
"""
Compress HTML files using gzip for embedding in firmware.
Usage: gzip_html.py <input_dir> <output_dir>
"""
import sys
import os
import gzip


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <input_dir> <output_dir>")
        sys.exit(1)

    input_dir = sys.argv[1]
    output_dir = sys.argv[2]

    os.makedirs(output_dir, exist_ok=True)

    print("-- HTML gzip compression:")
    for filename in os.listdir(input_dir):
        if filename.endswith('.html'):
            input_path = os.path.join(input_dir, filename)
            output_path = os.path.join(output_dir, filename + '.gz')

            with open(input_path, 'rb') as f_in:
                data = f_in.read()

            compressed = gzip.compress(data, compresslevel=9)

            with open(output_path, 'wb') as f_out:
                f_out.write(compressed)

            ratio = (1 - len(compressed) / len(data)) * 100
            print(f"  {filename}: {len(data)} -> {len(compressed)} bytes ({ratio:.1f}% reduction)")


if __name__ == '__main__':
    main()
