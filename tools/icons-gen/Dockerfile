# Rasteriser for the player's icon sets.
#
# tools/svg_to_lvgl.py in the sonix-player tree needs cairosvg and pillow, and
# neither is installed on the machine this is built from. The generated C is
# checked into the repo, so this image is only needed when an icon changes or
# the 320x480 set has to be produced again:
#
#   docker build -t sonix-icons tools/icons-gen
#   docker run --rm -v "$PWD":/work -w /work/sonix-player sonix-icons
#
# Defaults to the 320x480 set (--small-only); pass other arguments to override:
#   docker run --rm -v "$PWD":/work -w /work/sonix-player sonix-icons \
#       python3 tools/svg_to_lvgl.py --all
FROM python:3.12-slim

# libcairo2 is what cairosvg draws with; libffi8 is what cffi (cairocffi) binds
# through. Nothing else is needed: the SVG parsing side is pure Python.
RUN apt-get update \
	&& apt-get install -y --no-install-recommends libcairo2 libffi8 \
	&& rm -rf /var/lib/apt/lists/*

RUN pip install --no-cache-dir cairosvg pillow

WORKDIR /work
CMD ["python3", "tools/svg_to_lvgl.py", "--small-only"]
