FROM python:3.11-slim AS miniaudio-builder
RUN apt-get update \
    && apt-get install -y --no-install-recommends g++ \
    && pip wheel --no-cache-dir --wheel-dir /wheels miniaudio==1.71

FROM python:3.11-slim
WORKDIR /app
COPY --from=miniaudio-builder /wheels /wheels
RUN pip install --no-cache-dir openwakeword==0.6.0 /wheels/miniaudio-*.whl \
    && python -c "from openwakeword import utils; utils.download_models(['hey_jarvis'])"
COPY server.py dashboard.html ./
EXPOSE 8765 8766
CMD ["python", "-u", "/app/server.py"]
