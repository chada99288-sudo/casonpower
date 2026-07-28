FROM python:3.11-slim

WORKDIR /app

COPY src/requirements.txt /app/src/requirements.txt
RUN pip install --no-cache-dir -r /app/src/requirements.txt

COPY . /app

ENV PORT=8080
EXPOSE 8080

CMD ["python", "src/server.py"]
