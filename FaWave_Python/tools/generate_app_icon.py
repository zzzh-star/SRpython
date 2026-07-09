import sys
import os
try:
    from PIL import Image
except ImportError:
    print("Pillow library is not installed. Please run: pip install pillow")
    sys.exit(1)

def generate_ico():
    target_dir = os.path.join(os.path.dirname(__file__), '..', 'assets')
    os.makedirs(target_dir, exist_ok=True)

    # We will generate a generic fallback image dynamically if the SVG can't be converted purely natively
    ico_path = os.path.join(target_dir, 'app_icon.ico')

    # Let's create a generic blue wave icon programmatically
    img = Image.new('RGBA', (256, 256), (255, 255, 255, 0))
    # Simple blue square with white circle
    from PIL import ImageDraw
    draw = ImageDraw.Draw(img)
    draw.rounded_rectangle([(16, 16), (240, 240)], radius=40, fill=(37, 99, 235, 255))
    draw.ellipse([(64, 64), (192, 192)], fill=(255, 255, 255, 255))
    draw.line([(80, 128), (120, 80), (160, 160), (200, 128)], fill=(37, 99, 235, 255), width=20, joint="curve")

    # Save with multiple sizes spanning required Windows limits natively
    img.save(ico_path, format='ICO', sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    print(f"Successfully generated icon at {ico_path}")

if __name__ == "__main__":
    generate_ico()
