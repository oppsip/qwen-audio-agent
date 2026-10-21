import assert from 'node:assert/strict'
import test from 'node:test'
import {
  ResponseOutputPacer,
  base64ByteLength,
  pcm16Seconds,
} from '../src/voice/response-output-pacer.mjs'

function harness({ rate = 1, maxQueuedSeconds = 120 } = {}) {
  let clock = 0
  let nextTimer = 1
  const timers = new Map()
  const dropped = []
  const pacer = new ResponseOutputPacer({
    rate,
    maxQueuedSeconds,
    now: () => clock,
    setTimer: (callback, delay) => {
      const id = nextTimer
      nextTimer += 1
      timers.set(id, { at: clock + delay, callback })
      return id
    },
    clearTimer: id => timers.delete(id),
    onDrop: drop => dropped.push(drop),
  })
  const advance = milliseconds => {
    const target = clock + milliseconds
    for (;;) {
      const due = [...timers.entries()]
        .filter(([, timer]) => timer.at <= target)
        .sort((a, b) => a[1].at - b[1].at)[0]
      if (!due) break
      timers.delete(due[0])
      clock = due[1].at
      due[1].callback()
    }
    clock = target
  }
  // Move the wall clock without running timers, like an event loop stall.
  const stall = milliseconds => {
    clock += milliseconds
  }
  return { pacer, advance, stall, dropped, now: () => clock }
}

test('releases the first chunk immediately and paces the rest', () => {
  const { pacer, advance } = harness()
  const sent = []
  pacer.push({ seconds: 1, send: () => sent.push('a1') })
  assert.deepEqual(sent, ['a1'])
  pacer.push({ seconds: 0.5, send: () => sent.push('a2') })
  assert.deepEqual(sent, ['a1'])
  advance(999)
  assert.deepEqual(sent, ['a1'])
  advance(1)
  assert.deepEqual(sent, ['a1', 'a2'])
  assert.equal(pacer.queuedSeconds, 0)
})

test('events without audio wait for the audio queued before them', () => {
  const { pacer, advance } = harness()
  const sent = []
  pacer.push({ seconds: 1, send: () => sent.push('a1') })
  pacer.push({ send: () => sent.push('t1') })
  pacer.push({ seconds: 1, send: () => sent.push('a2') })
  assert.deepEqual(sent, ['a1'])
  advance(1000)
  assert.deepEqual(sent, ['a1', 't1', 'a2'])
})

test('a burst is released over its own playback duration', () => {
  const { pacer, advance, now } = harness()
  const sent = []
  for (let index = 0; index < 40; index += 1) {
    pacer.push({ seconds: 0.5, send: () => sent.push(now()) })
  }
  assert.equal(sent.length, 1)
  assert.equal(pacer.queuedSeconds, 19.5)
  advance(5000)
  assert.equal(sent.length, 11)
  assert.equal(pacer.queuedSeconds, 14.5)
})

test('flush drops queued audio and closes the pacer', () => {
  const { pacer, advance } = harness()
  const sent = []
  pacer.push({ seconds: 5, send: () => sent.push('a1') })
  pacer.push({ seconds: 5, send: () => sent.push('a2') })
  pacer.push({ send: () => sent.push('t1') })
  const result = pacer.flush('user_interruption')
  assert.deepEqual(result, {
    pendingItems: 2,
    droppedSeconds: 5,
    reason: 'user_interruption',
  })
  assert.equal(pacer.closed, true)
  assert.equal(pacer.push({ seconds: 1, send: () => sent.push('a3') }), false)
  advance(20_000)
  assert.deepEqual(sent, ['a1'])
})

test('overflow drops the oldest audio and keeps playing the newest', () => {
  const { pacer, advance, dropped } = harness({ maxQueuedSeconds: 2 })
  const sent = []
  pacer.push({ seconds: 1, send: () => sent.push('a1') })
  pacer.push({ seconds: 1, send: () => sent.push('a2') })
  pacer.push({ seconds: 1, send: () => sent.push('a3') })
  pacer.push({ seconds: 1, send: () => sent.push('a4') })
  assert.deepEqual(sent, ['a1', 'a3'])
  assert.deepEqual(dropped, [{ droppedSeconds: 1, queuedSeconds: 1 }])
  assert.equal(pacer.queuedSeconds, 1)
  advance(1000)
  assert.deepEqual(sent, ['a1', 'a3', 'a4'])
})

test('a slow consumer is paced at real time', () => {
  const { pacer, advance } = harness()
  const sent = []
  for (let index = 0; index < 10; index += 1) {
    pacer.push({ seconds: 0.02, send: () => sent.push(index) })
  }
  assert.equal(sent.length, 1)
  advance(180)
  assert.equal(sent.length, 10)
})

test('base64 payload lengths map to playback seconds', () => {
  // 20 ms of 24 kHz PCM16 is 960 bytes, 1280 base64 characters.
  const silence = Buffer.alloc(960).toString('base64')
  assert.equal(base64ByteLength(silence), 960)
  assert.equal(pcm16Seconds(silence, 24000), 0.02)
  assert.equal(pcm16Seconds(silence, 0), 0)
  assert.equal(pcm16Seconds('', 24000), 0)
  // Padding must not round up into a phantom frame.
  assert.equal(base64ByteLength(Buffer.alloc(2).toString('base64')), 2)
  assert.equal(base64ByteLength(Buffer.alloc(1).toString('base64')), 1)
})

test('a stalled timer catches up instead of drifting slower than real time', () => {
  const { pacer, advance, stall } = harness()
  const sent = []
  pacer.push({ seconds: 0.02, send: () => sent.push('a1') })
  pacer.push({ seconds: 0.02, send: () => sent.push('a2') })
  assert.equal(pacer.headAt, 40)
  stall(100)
  pacer.push({ seconds: 0.02, send: () => sent.push('a3') })
  // The wall-clock chain is unchanged: a3 still ends at 60 ms, not 120 ms.
  assert.equal(pacer.headAt, 60)
  advance(0)
  assert.deepEqual(sent, ['a1', 'a2', 'a3'])
})

test('an idle pacer restarts from now', () => {
  const { pacer, advance } = harness()
  const sent = []
  pacer.push({ seconds: 0.02, send: () => sent.push(0) })
  advance(5000)
  pacer.push({ seconds: 0.02, send: () => sent.push(1) })
  assert.deepEqual(sent, [0, 1])
})
