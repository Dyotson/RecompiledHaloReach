# Reach Live server: docker build -t reach-live server/ &&
#   docker run --network host -v reach-live-data:/data reach-live
# (or -p 21100:21100/udp -p 21101:21101; --network host keeps players' public ports intact)
FROM python:3.12-slim
WORKDIR /app
COPY *.py ./
EXPOSE 21100/udp 21101/tcp
VOLUME /data
ENTRYPOINT ["python3", "reach_live_server.py", "--port", "21100", "--http-port", "21101", "--data-dir", "/data"]
