# The API. Python and nothing else.
#
# One stage, because there is nothing to compile: the API never runs a
# simulation and has no use for the simulator. That is the point of it being a
# separate image from the worker's, which carries a C++ binary and the shared
# libraries that binary needs.
#
# The build context is the repository root, so that this file and the worker's
# sit beside each other and take the same context.

FROM python:3.12-slim-bookworm

WORKDIR /service
COPY service/pyproject.toml ./
COPY service/orrery_service/ orrery_service/
RUN pip install --no-cache-dir .

RUN useradd --create-home --uid 10001 api
USER api

EXPOSE 8000
ENTRYPOINT ["python", "-m", "orrery_service.api"]
