# Rasterises the interface's SVG icons: the environment tools/svg_to_lvgl.py
# needs, and nothing else.
#
# The generated files (src/gui/shell/icons_small.c and icons.h, icons_select.h)
# are checked in, so building the player never needs Python, cairosvg or a
# rasteriser. This image is for the day an icon changes or upstream adds one:
#
#   docker build -t sonix-icons -f sonix-player/docker/icons.Dockerfile .
#   docker run --rm -u "$(id -u):$(id -g)" -v "$PWD":/work \
#       -w /work/sonix-player sonix-icons
#
# cairosvg needs libcairo2 at run time; Pillow is what the script composes the
# PNG tiles with. The output is written into the mounted tree, as the calling
# user, so nothing in the repository ends up owned by root.
FROM python:3.12-slim

RUN apt-get update \
 && apt-get install -y --no-install-recommends libcairo2 \
 && rm -rf /var/lib/apt/lists/* \
 && pip install --no-cache-dir cairosvg pillow

WORKDIR /work

# The small set only: icons.c is upstream's, is not compiled on this board and
# is hundreds of thousands of lines to rewrite for nothing.
CMD ["python3", "tools/svg_to_lvgl.py", "--small-only"]
