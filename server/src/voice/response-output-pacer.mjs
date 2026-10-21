/**
 * Paces provider response output for one response.
 *
 * Realtime providers can deliver a whole answer far faster than real time
 * (DashScope bursts audio at ~3x). A client that plays audio at 1x has to hold
 * the difference somewhere; if it is the client, a barge-in throws away tens of
 * seconds it already accepted, and a slow client link can overflow the
 * Gateway audio budget. This pacer releases audio at roughly real time and
 * keeps the backlog here, where cancelling a response can drop it immediately.
 *
 * Ordering contract: items are released in enqueue order. An item with
 * `seconds > 0` occupies that much playback time; an item without audio is
 * released at the end of the audio queued before it, so transcripts trail the
 * voice they describe instead of running ahead of it.
 */

const DEFAULT_MAX_QUEUED_SECONDS = 120

export function base64ByteLength(encoded) {
  const length = encoded?.length || 0
  if (!length) return 0
  let padding = 0
  if (encoded.charCodeAt(length - 1) === 61) padding += 1
  if (length > 1 && encoded.charCodeAt(length - 2) === 61) padding += 1
  return Math.max(0, Math.floor((length * 3) / 4) - padding)
}

/** Playback duration of a base64 PCM16 payload. */
export function pcm16Seconds(encoded, sampleRate) {
  const rate = Number(sampleRate)
  if (!Number.isFinite(rate) || rate <= 0) return 0
  return base64ByteLength(encoded) / (rate * 2)
}

export class ResponseOutputPacer {
  #id
  #rate
  #maxQueuedSeconds
  #now
  #setTimer
  #clearTimer
  #onSendFailure
  #onDrop
  #queue = []
  #headAt = 0
  #queuedSeconds = 0
  #timer = null
  #closed = false
  #droppedSeconds = 0

  constructor({
    id = '',
    rate = 1,
    maxQueuedSeconds = DEFAULT_MAX_QUEUED_SECONDS,
    now = () => Date.now(),
    setTimer = (callback, delay) => setTimeout(callback, delay),
    clearTimer = handle => clearTimeout(handle),
    onSendFailure = null,
    onDrop = null,
  } = {}) {
    const parsedRate = Number(rate)
    const parsedMax = Number(maxQueuedSeconds)
    this.#id = String(id || '')
    this.#rate = parsedRate > 0 ? parsedRate : 1
    this.#maxQueuedSeconds = parsedMax > 0 ? parsedMax : DEFAULT_MAX_QUEUED_SECONDS
    this.#now = now
    this.#setTimer = setTimer
    this.#clearTimer = clearTimer
    this.#onSendFailure = onSendFailure
    this.#onDrop = onDrop
  }

  get id() {
    return this.#id
  }

  /** Closed pacers release nothing more; callers should send directly. */
  get closed() {
    return this.#closed
  }

  get queuedSeconds() {
    return this.#queuedSeconds
  }

  get pendingItems() {
    return this.#queue.length
  }

  get droppedSeconds() {
    return this.#droppedSeconds
  }

  /** Wall-clock end of the queued playback, in ms; 0 when idle. */
  get headAt() {
    return this.#headAt
  }

  /**
   * Queue one output item. `send` runs when the item's playback slot arrives;
   * `seconds` is the audio duration the item occupies (0 for text/control).
   */
  push({ seconds = 0, send } = {}) {
    if (this.#closed || typeof send !== 'function') return false
    const duration = Number(seconds) > 0 ? Number(seconds) : 0
    if (duration > 0 && this.#queuedSeconds + duration > this.#maxQueuedSeconds) {
      this.#dropOldest(this.#queuedSeconds + duration - this.#maxQueuedSeconds)
    }
    // While the queue is still draining, keep the original wall-clock chain:
    // clamping to "now" every push would let timer lateness accumulate and
    // release slightly slower than real time, starving the client. Only an
    // idle pacer re-anchors to now.
    const start = this.#queue.length
      ? this.#headAt
      : Math.max(this.#now(), this.#headAt)
    this.#queue.push({ at: start, seconds: duration, send })
    if (duration > 0) {
      this.#headAt = start + (duration * 1000) / this.#rate
      this.#queuedSeconds += duration
    }
    this.#pump()
    return true
  }

  /**
   * Drop everything still queued and close. Used when a response is cancelled,
   * the user barges in, or the session ends.
   */
  flush(reason = '') {
    let droppedSeconds = 0
    for (const item of this.#queue) {
      if (item.seconds > 0) droppedSeconds += item.seconds
    }
    const pendingItems = this.#queue.length
    this.#queue = []
    this.#queuedSeconds = 0
    this.#headAt = 0
    this.#closed = true
    this.#disarm()
    return { pendingItems, droppedSeconds, reason: String(reason || '') }
  }

  #pump() {
    const now = this.#now()
    if (this.#queue.length && this.#queue[0].at <= now) {
      this.#drain(now)
      return
    }
    this.#arm()
  }

  #drain(now) {
    this.#disarm()
    while (this.#queue.length && this.#queue[0].at <= now) {
      const item = this.#queue.shift()
      if (item.seconds > 0) {
        this.#queuedSeconds = Math.max(0, this.#queuedSeconds - item.seconds)
      }
      try {
        item.send()
      } catch (error) {
        this.#onSendFailure?.(error)
      }
    }
    this.#arm()
  }

  #arm() {
    if (this.#closed) return
    this.#disarm()
    if (!this.#queue.length) return
    const delay = Math.max(0, this.#queue[0].at - this.#now())
    this.#timer = this.#setTimer(() => {
      this.#timer = null
      if (this.#closed) return
      this.#drain(this.#now())
    }, delay)
    this.#timer?.unref?.()
  }

  #disarm() {
    if (this.#timer === null) return
    this.#clearTimer(this.#timer)
    this.#timer = null
  }

  #dropOldest(excessSeconds) {
    let dropped = 0
    const kept = []
    for (const item of this.#queue) {
      if (dropped < excessSeconds && item.seconds > 0) {
        dropped += item.seconds
        continue
      }
      kept.push(item)
    }
    if (!dropped) return
    this.#queuedSeconds = Math.max(0, this.#queuedSeconds - dropped)
    this.#droppedSeconds += dropped
    // Skip ahead: the survivors keep their spacing but start from now.
    let cursor = this.#now()
    for (const item of kept) {
      item.at = cursor
      if (item.seconds > 0) cursor += (item.seconds * 1000) / this.#rate
    }
    this.#queue = kept
    this.#headAt = cursor
    this.#onDrop?.({ droppedSeconds: dropped, queuedSeconds: this.#queuedSeconds })
  }
}
