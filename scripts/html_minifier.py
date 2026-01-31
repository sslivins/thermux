#!/usr/bin/env python3
"""
HTML/CSS/JS Minifier for ESP32 Web Server
Uses minify-html library for reliable minification.
"""

import sys
import os

try:
    import minify_html
    HAS_MINIFY = True
except ImportError:
    HAS_MINIFY = False
    print("Warning: minify-html not installed. Run: pip install minify-html")


def minify_file(content):
    """Minify HTML content with embedded CSS and JS."""
    if not HAS_MINIFY:
        return content
    
    return minify_html.minify(
        content,
        minify_js=True,
        minify_css=True,
        remove_processing_instructions=True,
        minify_doctype=True,
        keep_closing_tags=True,
        keep_html_and_head_opening_tags=True,
        remove_bangs=False,
    )


def main():
    if len(sys.argv) < 3:
        print("Usage: minify_html.py <input_dir> <output_dir>")
        sys.exit(1)
    
    input_dir = sys.argv[1]
    output_dir = sys.argv[2]
    
    # Create output directory if it doesn't exist
    os.makedirs(output_dir, exist_ok=True)
    
    # Process all HTML files
    print("-- HTML minification:")
    for filename in os.listdir(input_dir):
        if filename.endswith('.html'):
            input_path = os.path.join(input_dir, filename)
            output_path = os.path.join(output_dir, filename)
            
            with open(input_path, 'r', encoding='utf-8') as f:
                original = f.read()
            
            minified = minify_file(original)
            
            with open(output_path, 'w', encoding='utf-8') as f:
                f.write(minified)
            
            original_size = len(original.encode('utf-8'))
            minified_size = len(minified.encode('utf-8'))
            reduction = (1 - minified_size / original_size) * 100 if original_size > 0 else 0
            
            print(f"  {filename}: {original_size} -> {minified_size} bytes ({reduction:.1f}% reduction)")


if __name__ == '__main__':
    main()
