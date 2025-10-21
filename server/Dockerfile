# Build stage
FROM ubuntu:22.04 AS build
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential cmake git wget curl ca-certificates \
    libboost-dev libboost-system-dev libboost-thread-dev \
    libssl-dev nlohmann-json3-dev

WORKDIR /src
COPY beast_ws_chat_server.cpp /src/beast_ws_chat_server.cpp

RUN g++ -std=c++17 beast_ws_chat_server.cpp -O2 -lboost_system -lpthread -o /ws_chat_server

# Runtime stage
FROM ubuntu:22.04
COPY --from=build /ws_chat_server /usr/local/bin/ws_chat_server
EXPOSE 8080
CMD ["/usr/local/bin/ws_chat_server", "8080"]
