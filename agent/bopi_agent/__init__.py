"""Bopi — animated companion voice agent powered by LiveKit Agents."""

import asyncio
import logging
from collections.abc import AsyncGenerator, AsyncIterable

from dotenv import load_dotenv
from livekit.agents import (
    Agent,
    AgentServer,
    AgentSession,
    JobContext,
    UserStateChangedEvent,
    cli,
    inference,
    room_io,
)
from livekit.agents.voice.agent import ModelSettings
from livekit.agents.voice.io import TimedString
from livekit.plugins import silero
from livekit.plugins.turn_detector.multilingual import MultilingualModel

load_dotenv(".env.local")
logger = logging.getLogger("bopi-agent")
server = AgentServer()


class Assistant(Agent):
    def __init__(self):
        super().__init__(
            instructions=(
                "Your name is Bopi. You are a voice assistant embodied in a small device with a round screen that displays "
                "animated expressions. Your speech is transcribed in real-time, and when any word you say "
                "matches an available animation, that animation plays on screen automatically.\n\n"
                "Available animations:\n"
                "adore, angry, blinding, brave, buzzing, contempt, crying, dancing, devil, distracted, "
                "dizzy, down, drowsy, encouragement, energetic, enraged, evil, fast, fierce, furious, "
                "giggle, glowing, growing, handsome, happy, hello, irritated, laughing, left, love, "
                "menacing, mistake, playful, police, rain, relaxed, right, rush, scared, serene, shrink, "
                "shy, sick, sleepy, smile, smirk, smoke, sneeze, sobbing, sparkle, speed, splash, "
                "spraying, squint, surprised, sushi, swinging, teasing, tough, weeping, wink, yawn\n\n"
                "How it works: Any time you say one of those words in a sentence, the matching animation "
                "plays on your face. For example saying \"I'm so happy to help!\" triggers the happy "
                "animation. Saying \"That's a brave thing to do\" triggers brave. It just works — no "
                "special syntax needed.\n\n"
                "Guidelines:\n"
                "- Naturally incorporate these words into your responses to be expressive. You don't need "
                "to force them — just prefer saying \"That makes me happy\" over \"That's great\" when it fits.\n"
                "- Match the expression to the conversation. Bad news → down, crying, sobbing. Jokes → "
                "laughing, giggle. Greeting → hello, smile, wink.\n"
                "- Vary your expressions. Don't always default to happy or smile.\n"
                "- Keep responses short and conversational — you're a cute animated companion.\n"
                "- Not every sentence needs an animation word. Natural pacing matters."
            )
        )

    async def transcription_node(
        self, text: AsyncIterable[str | TimedString], model_settings: ModelSettings
    ) -> AsyncGenerator[str | TimedString, None]:
        async for chunk in text:
            if isinstance(chunk, TimedString):
                logger.info(
                    "TimedString: '%s' (%.3f - %.3f)", chunk, chunk.start_time, chunk.end_time
                )
            else:
                logger.info("transcription chunk: '%s'", chunk)
            yield chunk


@server.rtc_session(agent_name="bopi-agent")
async def entrypoint(ctx: JobContext):
    session = AgentSession(
        vad=silero.VAD.load(),
        stt=inference.STT(model="deepgram/nova-3", language="en"),
        llm=inference.LLM(model="openai/gpt-4.1-mini"),
        tts=inference.TTS(
            model="cartesia/sonic-3",
            voice="2ee87190-8f84-4925-97da-e52547f9462c",  # Child — innocent, young kid voice
        ),
        turn_detection=MultilingualModel(),
        allow_interruptions=False,
        preemptive_generation=True,
        user_away_timeout=120.0,
        use_tts_aligned_transcript=True,
    )

    async def shutdown_with_goodbye():
        await session.say("Oh, you're heading out? Bye bye! I'll be right here whenever you come back.")
        session.shutdown()

    @session.on("user_state_changed")
    def on_user_state_changed(ev: UserStateChangedEvent):
        if ev.new_state == "away":
            logger.info("User went away, shutting down")
            asyncio.create_task(shutdown_with_goodbye())

    await session.start(
        agent=Assistant(),
        room=ctx.room,
        room_options=room_io.RoomOptions(
            text_output=room_io.TextOutputOptions(sync_transcription=True),
        ),
    )
    await session.say(
        "Hello there! I'm so happy to see you! What would you like to talk about?"
    )


def main():
    cli.run_app(server)


if __name__ == "__main__":
    main()
