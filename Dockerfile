FROM gcc:latest AS builder

WORKDIR /build
COPY . .

RUN apt-get update && apt-get install -y make
RUN make

# Runtime
FROM debian:bookworm-slim

RUN useradd -r -s /bin/false kvuser
WORKDIR /app

# Copy the binary that the Makefile generated
COPY --from=builder /build/kvstore /app/kvstore

RUN chown -R kvuser:kvuser /app/
USER kvuser
EXPOSE 6379

CMD ["./kvstore"]