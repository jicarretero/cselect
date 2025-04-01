FROM alpine:latest AS build

# update certificates to trust github
COPY ./select.c /tmp/select.c
COPY ./makefile /tmp/makefile

WORKDIR /tmp
RUN  apk add --no-cache gcc musl-dev make && \
  make

FROM alpine:latest
COPY --from=build /tmp/select /usr/bin/select
RUN apk add wget curl bind-tools net-tools

ENTRYPOINT [ "/usr/bin/select" ]
