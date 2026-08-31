FROM python:3.11-slim AS miniaudio-builder
RUN apt-get update \
    && apt-get install -y --no-install-recommends g++ \
    && pip wheel --no-cache-dir --wheel-dir /wheels miniaudio==1.71

FROM python:3.11-slim
WORKDIR /app
COPY --from=miniaudio-builder /wheels /wheels
RUN pip install --no-cache-dir "aiohttp>=3.11,<4" openwakeword==0.6.0 /wheels/miniaudio-*.whl \
    && python -c "from openwakeword import utils; utils.download_models(['hey_jarvis'])"
COPY server.py dashboard.html ./
EXPOSE 8766
HEALTHCHECK --interval=30s --timeout=4s --start-period=20s --retries=3 \
    CMD python -c "import urllib.request; urllib.request.urlopen('http://127.0.0.1:8766/health', timeout=2).read()" || exit 1
CMD ["python", "-u", "/app/server.py"]
