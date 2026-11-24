"""Embedding model selection (Cohere by default, Titan where Cohere is not available)."""

from __future__ import annotations

import json

import pytest

from app.ai import client
from app.config import get_settings


@pytest.fixture
def embed_model(monkeypatch: pytest.MonkeyPatch):  # type: ignore[no-untyped-def]
    def use(model_id: str) -> client.Embedder:
        monkeypatch.setenv("BEDROCK_EMBED_MODEL_ID", model_id)
        monkeypatch.setenv("AWS_DEFAULT_REGION", "ap-south-1")
        get_settings.cache_clear()
        client.get_embedder.cache_clear()
        return client.get_embedder()

    yield use
    get_settings.cache_clear()
    client.get_embedder.cache_clear()


def test_cohere_is_the_default_embedder(embed_model) -> None:  # type: ignore[no-untyped-def]
    assert isinstance(embed_model("cohere.embed-multilingual-v3"), client.BedrockCohereEmbedder)


def test_titan_embedder_requests_the_database_dimension(embed_model) -> None:  # type: ignore[no-untyped-def]
    embedder = embed_model("amazon.titan-embed-text-v2:0")
    assert isinstance(embedder, client.BedrockTitanEmbedder)
    body = json.loads(embedder._body("CPU scheduling"))
    assert body == {"inputText": "CPU scheduling", "dimensions": 1024, "normalize": True}


class ScriptedLlm:
    def __init__(self, name: str, reply: str | None) -> None:
        self.model_name = name
        self.reply = reply
        self.calls = 0

    async def generate(self, system: str, user: str, schema: type[client.T]) -> client.T:
        self.calls += 1
        if self.reply is None:
            raise client.AiUnavailableError("Model access is denied due to INVALID_PAYMENT_INSTRUMENT")
        return schema.model_validate_json(self.reply)


def test_fallback_uses_the_next_model_when_the_best_one_is_refused() -> None:
    import asyncio

    from app.ai.schemas import LiveKeyTerms

    reply = '{"terms": [{"term": "quantum", "definition": "A CPU time slice."}]}'
    claude, nova = ScriptedLlm("claude", None), ScriptedLlm("nova", reply)
    llm = client.FallbackLlm([claude, nova])
    out = asyncio.run(llm.generate("s", "u", LiveKeyTerms))
    assert out.terms[0].term == "quantum" and llm.model_name == "nova"  # recorded with the notes
    claude.reply = reply
    asyncio.run(llm.generate("s", "u", LiveKeyTerms))
    assert llm.model_name == "claude" and nova.calls == 1  # the best model is tried first every time

    nobody = client.FallbackLlm([ScriptedLlm("a", None), ScriptedLlm("b", None)])
    with pytest.raises(client.AiUnavailableError, match=r"a: .*b: "):
        asyncio.run(nobody.generate("s", "u", LiveKeyTerms))


def test_a_comma_separated_model_list_builds_a_fallback_chain(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv(
        "BEDROCK_LLM_MODEL_ID", "global.anthropic.claude-opus-4-6-v1, apac.amazon.nova-lite-v1:0"
    )
    monkeypatch.setenv("AWS_DEFAULT_REGION", "ap-south-1")
    get_settings.cache_clear()
    client.get_llm.cache_clear()
    try:
        llm = client.get_llm()
        assert isinstance(llm, client.FallbackLlm)
        assert llm.model_name == "global.anthropic.claude-opus-4-6-v1"
    finally:
        get_settings.cache_clear()
        client.get_llm.cache_clear()
