# A region's directory (docs/SCALING.md): stateless, any number of replicas on one Redis.
FROM python:3.12-slim
RUN useradd --system --uid 10001 directory
WORKDIR /opt/directory
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY directory.py .
USER directory
ENV PORT=8080 REDIS_URL=redis://redis:6379/0
EXPOSE 8080
HEALTHCHECK --interval=10s --timeout=3s CMD python3 -c "import urllib.request,os; urllib.request.urlopen('http://127.0.0.1:%s/healthz' % os.environ['PORT'], timeout=2)"
CMD ["python3", "directory.py"]
