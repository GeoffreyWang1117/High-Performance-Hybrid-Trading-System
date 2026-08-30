"""Minimal OpenAI-compatible chat endpoint backed by transformers on CPU.

WHY THIS EXISTS
---------------
titans_llm_experiment talks to an OpenAI-compatible /v1/chat/completions
endpoint (see VLLMBackendImpl in include/titans/context/ollama_backend.hpp).
Normally that is served by vLLM on a GPU. Two situations leave no GPU:

  - CI, which has none at all;
  - a shared workstation whose GPUs are fully committed to other jobs, where
    grabbing memory would OOM someone else's run.

This serves the same protocol on CPU so the experiment path stays exercisable
in both. It is deliberately small and NOT a vLLM replacement: no batching, no
paged attention, no streaming, one request at a time. Expect single-digit
tokens/sec on a desktop CPU, which is fine for a few hundred short structured
answers and useless for anything larger.

Usage:
    python cpu_shim.py --model Qwen/Qwen2.5-1.5B-Instruct --port 8011

Then point the experiment at it:
    titans_llm_experiment --backend vllm --port 8011
"""

from __future__ import annotations

import argparse
import logging
import time
import uuid
from typing import Any

import torch
import uvicorn
from fastapi import FastAPI
from pydantic import BaseModel, Field
from transformers import AutoModelForCausalLM, AutoTokenizer

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
log = logging.getLogger("cpu_shim")


class ChatMessage(BaseModel):
    role: str
    content: str


class ChatRequest(BaseModel):
    model: str = ""
    messages: list[ChatMessage]
    temperature: float = 0.0
    max_tokens: int = 256
    stream: bool = False
    # Accepted and ignored; the C++ client may send it.
    response_format: Any = None


class _Engine:
    """Holds the model. Loaded once, reused across requests."""

    def __init__(self, model_id: str, threads: int) -> None:
        torch.set_num_threads(threads)
        log.info("loading %s on CPU with %d threads", model_id, threads)
        t0 = time.time()
        self.tokenizer = AutoTokenizer.from_pretrained(model_id)
        # No device_map: that path requires `accelerate`, and this shim is
        # single-device by construction. Plain .to("cpu") keeps the dependency
        # surface to transformers + torch, which CI already needs.
        self.model = AutoModelForCausalLM.from_pretrained(
            model_id,
            dtype=torch.float32,  # CPU has no useful bf16 matmul path here
        ).to("cpu")
        self.model.eval()
        self.model_id = model_id
        log.info("loaded in %.1f s", time.time() - t0)

    @torch.inference_mode()
    def generate(self, messages: list[ChatMessage], max_tokens: int,
                 temperature: float) -> tuple[str, int, int]:
        prompt = self.tokenizer.apply_chat_template(
            [{"role": m.role, "content": m.content} for m in messages],
            tokenize=False,
            add_generation_prompt=True,
        )
        inputs = self.tokenizer(prompt, return_tensors="pt")
        prompt_tokens = int(inputs["input_ids"].shape[1])

        out = self.model.generate(
            **inputs,
            max_new_tokens=max_tokens,
            # temperature=0 means greedy; sampling with T=0 is undefined and
            # transformers warns about it, so branch explicitly.
            do_sample=temperature > 0.0,
            temperature=temperature if temperature > 0.0 else None,
            top_p=None if temperature <= 0.0 else 0.95,
            pad_token_id=self.tokenizer.eos_token_id,
        )
        generated = out[0][prompt_tokens:]
        text = self.tokenizer.decode(generated, skip_special_tokens=True)
        return text, prompt_tokens, int(generated.shape[0])


def build_app(engine: _Engine) -> FastAPI:
    app = FastAPI(title="titans cpu inference shim")

    @app.get("/v1/models")
    def list_models() -> dict:
        return {
            "object": "list",
            "data": [{"id": engine.model_id, "object": "model",
                      "owned_by": "local"}],
        }

    @app.get("/health")
    def health() -> dict:
        return {"status": "ok", "model": engine.model_id, "device": "cpu"}

    @app.post("/v1/chat/completions")
    def chat(req: ChatRequest) -> dict:
        t0 = time.time()
        text, ptok, ctok = engine.generate(req.messages, req.max_tokens,
                                           req.temperature)
        dt = time.time() - t0
        log.info("completion: %d prompt + %d completion tokens in %.2f s "
                 "(%.1f tok/s)", ptok, ctok, dt, ctok / dt if dt > 0 else 0.0)
        return {
            "id": f"chatcmpl-{uuid.uuid4().hex[:12]}",
            "object": "chat.completion",
            "created": int(time.time()),
            "model": engine.model_id,
            "choices": [{
                "index": 0,
                "message": {"role": "assistant", "content": text},
                "finish_reason": "stop",
            }],
            "usage": {
                "prompt_tokens": ptok,
                "completion_tokens": ctok,
                "total_tokens": ptok + ctok,
            },
        }

    return app


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--model", default="Qwen/Qwen2.5-1.5B-Instruct")
    ap.add_argument("--port", type=int, default=8011)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--threads", type=int, default=16,
                    help="torch CPU threads; leave headroom for other jobs")
    args = ap.parse_args()

    engine = _Engine(args.model, args.threads)
    uvicorn.run(build_app(engine), host=args.host, port=args.port,
                log_level="warning")


if __name__ == "__main__":
    main()
