"""LLM and embedding clients (D9: Amazon Bedrock). The browser never talks to Bedrock.

Both are behind small protocols so the notes worker and the API can be tested with fakes.
"""

from __future__ import annotations

import asyncio
import json
import re
from functools import lru_cache
from typing import Any, Literal, Protocol, TypeVar

from pydantic import BaseModel, ValidationError

from app.config import get_settings
from app.logs import get_logger

T = TypeVar("T", bound=BaseModel)
log = get_logger("ai")


class AiUnavailableError(RuntimeError):
    """No model configured, or the provider failed."""


class LlmClient(Protocol):
    model_name: str

    async def generate(self, system: str, user: str, schema: type[T]) -> T: ...


class Embedder(Protocol):
    model_name: str

    async def embed(self, texts: list[str], kind: Literal["document", "query"]) -> list[list[float]]: ...


def extract_json(text: str) -> str:
    """Takes the JSON object out of a reply (models sometimes add a code fence or a sentence)."""
    fenced = re.search(r"```(?:json)?\s*(\{.*\})\s*```", text, re.DOTALL)
    if fenced:
        return fenced.group(1)
    start, end = text.find("{"), text.rfind("}")
    if start == -1 or end <= start:
        raise ValueError("no JSON object in model output")
    return text[start : end + 1]


async def generate_validated(call: Any, system: str, user: str, schema: type[T]) -> T:
    """Calls the model and validates; on failure retries once, telling the model what was wrong."""
    prompt = user
    for attempt in range(2):
        raw: str = await call(system, prompt)
        try:
            # strict=False: models often put raw line breaks or tabs inside JSON strings (seen with
            # Amazon Nova on a Telugu lecture: notes with real newlines). They are unambiguous there.
            return schema.model_validate(json.loads(extract_json(raw), strict=False))
        except (ValidationError, ValueError) as exc:
            if attempt == 1:
                raise AiUnavailableError(f"model output failed validation: {exc}") from exc
            prompt = (
                f"{user}\n\nYour previous reply was invalid ({str(exc)[:500]}). "
                "Reply with only the corrected JSON object."
            )
    raise AssertionError("unreachable")


class BedrockLlm:
    def __init__(self, model_id: str, region: str) -> None:
        import boto3

        if not model_id:
            raise AiUnavailableError("BEDROCK_LLM_MODEL_ID is not set")
        self.model_name = model_id
        self._client = boto3.client("bedrock-runtime", region_name=region)

    async def _call(self, system: str, user: str) -> str:
        def invoke() -> str:
            response = self._client.converse(
                modelId=self.model_name,
                system=[{"text": system}],
                messages=[{"role": "user", "content": [{"text": user}]}],
                inferenceConfig={"maxTokens": 4096, "temperature": 0.2},
            )
            parts = response["output"]["message"]["content"]
            return "".join(part.get("text", "") for part in parts)

        try:
            return await asyncio.to_thread(invoke)
        except Exception as exc:  # botocore errors: throttling, access denied, model not enabled
            raise AiUnavailableError(f"Bedrock call failed: {exc}") from exc

    async def generate(self, system: str, user: str, schema: type[T]) -> T:
        return await generate_validated(self._call, system, user, schema)


class BedrockCohereEmbedder:
    """cohere.embed-multilingual-v3: 1024 dims, supports Hindi and Telugu, ≤ 96 texts per call."""

    BATCH = 96

    def __init__(self, model_id: str, region: str) -> None:
        import boto3

        self.model_name = model_id
        self._client = boto3.client("bedrock-runtime", region_name=region)

    async def embed(self, texts: list[str], kind: Literal["document", "query"]) -> list[list[float]]:
        input_type = "search_document" if kind == "document" else "search_query"

        def invoke(batch: list[str]) -> list[list[float]]:
            body = json.dumps(
                {"texts": [t[:2048] for t in batch], "input_type": input_type, "truncate": "END"}
            )
            response = self._client.invoke_model(
                modelId=self.model_name, body=body, contentType="application/json", accept="application/json"
            )
            embeddings: list[list[float]] = json.loads(response["body"].read())["embeddings"]
            return embeddings

        out: list[list[float]] = []
        try:
            for i in range(0, len(texts), self.BATCH):
                out.extend(await asyncio.to_thread(invoke, texts[i : i + self.BATCH]))
        except Exception as exc:
            raise AiUnavailableError(f"Bedrock embedding failed: {exc}") from exc
        return out


class BedrockTitanEmbedder:
    """amazon.titan-embed-text-v2:0 at 1024 dims, one text per call. For accounts where the Cohere
    model is unavailable (it is sold through AWS Marketplace); Cohere is preferred for Hindi/Telugu."""

    def __init__(self, model_id: str, region: str, dims: int) -> None:
        import boto3

        self.model_name = model_id
        self.dims = dims
        self._client = boto3.client("bedrock-runtime", region_name=region)

    def _body(self, text: str) -> str:
        return json.dumps({"inputText": text[:20_000], "dimensions": self.dims, "normalize": True})

    async def embed(self, texts: list[str], kind: Literal["document", "query"]) -> list[list[float]]:
        def invoke(text: str) -> list[float]:
            response = self._client.invoke_model(
                modelId=self.model_name,
                body=self._body(text),
                contentType="application/json",
                accept="application/json",
            )
            embedding: list[float] = json.loads(response["body"].read())["embedding"]
            return embedding

        try:
            return [await asyncio.to_thread(invoke, text) for text in texts]
        except Exception as exc:
            raise AiUnavailableError(f"Bedrock embedding failed: {exc}") from exc


class FallbackLlm:
    """Tries several models in order, e.g. a Claude model first and an Amazon Nova model second.

    A model that is refused (not enabled, no AWS Marketplace subscription, throttled) or whose reply
    still fails validation after its retry hands the request to the next one, so lectures still get
    notes while the preferred model is unavailable. `model_name` is the model that answered last,
    which is what the notes worker records with each artifact.
    """

    def __init__(self, llms: list[LlmClient]) -> None:
        if not llms:
            raise AiUnavailableError("BEDROCK_LLM_MODEL_ID is not set")
        self._llms = llms
        self.model_name = llms[0].model_name

    async def generate(self, system: str, user: str, schema: type[T]) -> T:
        failures: list[str] = []
        for llm in self._llms:
            try:
                out = await llm.generate(system, user, schema)
            except AiUnavailableError as exc:
                failures.append(f"{llm.model_name}: {exc}")
                log.warning("llm_unavailable_trying_next", model=llm.model_name, error=str(exc)[:300])
                continue
            self.model_name = llm.model_name
            return out
        raise AiUnavailableError("; ".join(failures))


@lru_cache
def get_llm() -> LlmClient:
    """BEDROCK_LLM_MODEL_ID may list several models, best first, separated by commas."""
    settings = get_settings()
    ids = [m.strip() for m in settings.bedrock_llm_model_id.split(",") if m.strip()]
    if len(ids) <= 1:
        return BedrockLlm(ids[0] if ids else "", settings.aws_region)
    return FallbackLlm([BedrockLlm(m, settings.aws_region) for m in ids])


@lru_cache
def get_embedder() -> Embedder:
    settings = get_settings()
    model_id = settings.bedrock_embed_model_id
    if model_id.startswith("amazon.titan-embed"):
        return BedrockTitanEmbedder(model_id, settings.aws_region, settings.embedding_dims)
    return BedrockCohereEmbedder(model_id, settings.aws_region)
