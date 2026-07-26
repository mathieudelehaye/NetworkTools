import asyncio
import aiohttp
from aiortc import RTCPeerConnection, RTCSessionDescription

async def test_whep():
    pc = RTCPeerConnection()
    pc.addTransceiver("video", direction="recvonly")

    @pc.on("track")
    def on_track(track):
        print(f" Success! Receiving live track from MCU: {track.kind}")

    # Generate SDP Offer
    await pc.setLocalDescription(await pc.createOffer())

    # Send WHEP POST request to Pion
    async with aiohttp.ClientSession() as session:
        async with session.post(
            "http://127.0.0.1:8080/whep",
            data=pc.localDescription.sdp,
            headers={"Content-Type": "application/sdp"}
        ) as resp:
            if resp.status in (200, 201):
                answer_sdp = await resp.text()
                await pc.setRemoteDescription(
                    RTCSessionDescription(sdp=answer_sdp, type="answer")
                )
                print(" WHEP WebRTC Handshake Completed Successfully!")
                print("Receiving video frames for 5 seconds...")
                await asyncio.sleep(5)
            else:
                print(f" Failed with status: {resp.status}")

asyncio.run(test_whep())