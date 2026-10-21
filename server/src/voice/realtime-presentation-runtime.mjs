import { GatewayServerEvent } from '../../../shared/protocol/realtime-events.mjs'
import {
  ensureResponseContext,
  mergeResponseContext,
  responseActivityContextPatch,
} from './response-context.mjs'
import {
  evaluateResponseGuards,
  isResponseGuardTurnCurrent,
} from './response-guards/index.mjs'
import {
  containsReservedProtocolEnvelope,
} from './response-guards/reserved-protocol-envelope.mjs'
import { realtimeResponseId } from './response-lifecycle.mjs'
import { ResponseOutputPacer, pcm16Seconds } from './response-output-pacer.mjs'

const PRESENTATION_RESPONSE_EVENTS = new Set([
  'response.created',
  'response.audio.delta',
  'response.output_audio.delta',
  'response.audio_transcript.delta',
  'response.output_audio_transcript.delta',
  'response.audio_transcript.done',
  'response.output_audio_transcript.done',
  'response.text.delta',
  'response.text.done',
  'response.done',
])

export function confirmsTaskNotificationOnPlaybackStart(context) {
  return Boolean(
    context
    && (
      context.origin === 'announcement'
      || context.consumesTaskNotification
    ),
  )
}

export function acceptsPlaybackReceipt({
  outputEnabled,
  active,
  responseKnown,
}) {
  return outputEnabled === true && active === true && responseKnown === true
}

function contextTaskIds(context) {
  return context?.taskIds?.length
    ? context.taskIds
    : [context?.taskId].filter(Boolean)
}

function awaitsToolFollowUp(context) {
  return Boolean(context?.awaitsToolFollowUp ?? context?.hasFunctionCall)
}

/**
 * Owns response correlation and user-visible presentation for one realtime
 * connection. Audio/text provider events and client playback receipts meet at
 * this boundary so a response is acknowledged, retried and recorded once.
 */
export class RealtimePresentationRuntime {
  constructor({
    ownerId,
    sessionId,
    turns,
    conversationSync,
    announcementWindow,
    announcements,
    toolCalls,
    send,
    getFrontend,
    getOutputEnabled,
    getNonVoiceClient,
    getResponseTurnCandidate,
    clearResponseCandidate,
    announcementQuietMs,
    responseContextCleanupMs,
    turnCitations = null,
    outputPacing = null,
    onResponseAudioStats = () => {},
  }) {
    this.ownerId = ownerId
    this.sessionId = sessionId
    this.turns = turns
    this.conversationSync = conversationSync
    this.announcementWindow = announcementWindow
    this.announcements = announcements
    this.toolCalls = toolCalls
    this.send = send
    this.getFrontend = getFrontend
    this.getOutputEnabled = getOutputEnabled
    this.getNonVoiceClient = getNonVoiceClient
    this.getResponseTurnCandidate = getResponseTurnCandidate
    this.clearResponseCandidate = clearResponseCandidate
    this.announcementQuietMs = announcementQuietMs
    this.responseContextCleanupMs = responseContextCleanupMs
    this.turnCitations = turnCitations
    this.onResponseAudioStats = onResponseAudioStats
    this.outputPacing = outputPacing && outputPacing.enabled !== false
      ? {
          rate: Number(outputPacing.rate) > 0 ? Number(outputPacing.rate) : 1,
          maxQueuedSeconds: Number(outputPacing.maxQueuedSeconds) > 0
            ? Number(outputPacing.maxQueuedSeconds)
            : 120,
          now: outputPacing.now,
          setTimer: outputPacing.setTimer,
          clearTimer: outputPacing.clearTimer,
        }
      : null
    this.pacers = new Map()
    this.audioStats = new Map()
    this.contexts = new Map()
    this.playbackTurns = new Map()
    this.lastCorrectionTurn = null
  }

  has(id) {
    return this.contexts.has(id)
  }

  get(id) {
    return this.contexts.get(id)
  }

  entries() {
    return this.contexts.entries()
  }

  markFunctionCall(id) {
    const context = this.contexts.get(id)
    if (context) {
      context.hasFunctionCall = true
      this.send({
        type: GatewayServerEvent.VOICE_STATE,
        state: 'processing',
        turnId: context.turnId || this.turns.turnId,
        origin: context.origin || 'model',
      })
    }
    return context
  }

  publicContext(context = {}) {
    return {
      turnId: context.turnId,
      taskId: context.taskId,
      taskIds: context.taskIds,
      origin: context.origin,
      turnGeneration: context.turnGeneration,
    }
  }

  fallbackContext() {
    return {
      turnId: this.turns.committedTurnId || this.turns.turnId,
      taskId: null,
      origin: 'model',
      turnGeneration: this.turns.committedTurnId
        ? this.turns.committedTurnGeneration
        : this.turns.turnGeneration,
    }
  }

  begin(event) {
    const id = realtimeResponseId(event)
    if (!id) return null
    const existing = this.contexts.get(id)
    const automaticResponse = (
      !existing
      && (event.__voiceOrigin || 'model') === 'model'
      && !event.__voiceContext?.turnId
    )
    const automaticTurn = automaticResponse
      ? this.getResponseTurnCandidate()
      : null
    const fallback = {
      turnId: event.__voiceContext?.turnId
        || automaticTurn?.turnId
        || this.turns.committedTurnId
        || this.turns.turnId,
      taskId: event.__voiceContext?.taskId || null,
      origin: event.__voiceOrigin || 'model',
      authorizationId: event.__voiceContext?.authorizationId || null,
      turnGeneration: Number.isInteger(event.__voiceContext?.turnGeneration)
        ? event.__voiceContext.turnGeneration
        : automaticTurn?.turnGeneration
          ?? (this.turns.committedTurnId
            ? this.turns.committedTurnGeneration
            : this.turns.turnGeneration),
    }
    const context = mergeResponseContext(
      this.contexts,
      id,
      responseActivityContextPatch({ existing, event, fallback }),
    )
    if (
      this.turns.manualInputGeneration !== null
      && !automaticResponse
      && context.turnGeneration === this.turns.manualInputGeneration
      && context.origin === 'model'
    ) {
      this.turns.finishManualResponse(context)
    }
    if (
      context.playbackStarted
      && confirmsTaskNotificationOnPlaybackStart(context)
    ) {
      this.announcements.confirmMany(contextTaskIds(context))
    }
    if (automaticTurn) {
      this.turns.commit(automaticTurn)
      this.clearResponseCandidate()
    }
    if (!context.responseStarted && !context.suppressed) {
      context.responseStarted = true
      this.send({
        type: GatewayServerEvent.RESPONSE_STARTED,
        responseId: id,
        ...this.publicContext(context),
      })
    }
    return context
  }

  handle(event) {
    if (!PRESENTATION_RESPONSE_EVENTS.has(event?.type)) return false
    if (event.type === 'response.created') return true
    if (
      event.type === 'response.audio.delta'
      || event.type === 'response.output_audio.delta'
    ) {
      this.#audioDelta(event)
    } else if (
      event.type === 'response.audio_transcript.delta'
      || event.type === 'response.output_audio_transcript.delta'
    ) {
      this.#audioTranscriptDelta(event)
    } else if (
      event.type === 'response.audio_transcript.done'
      || event.type === 'response.output_audio_transcript.done'
    ) {
      this.#audioTranscriptDone(event)
    } else if (event.type === 'response.text.delta') {
      this.#textDelta(event)
    } else if (event.type === 'response.text.done') {
      this.#textDone(event)
    } else if (event.type === 'response.done') {
      this.#responseDone(event)
    }
    return true
  }

  #contextFor(event) {
    return ensureResponseContext(
      this.contexts,
      realtimeResponseId(event),
      this.fallbackContext(),
    )
  }

  #pacerFor(id) {
    if (!this.outputPacing || !id) return null
    let pacer = this.pacers.get(id)
    if (!pacer) {
      pacer = new ResponseOutputPacer({
        id,
        rate: this.outputPacing.rate,
        maxQueuedSeconds: this.outputPacing.maxQueuedSeconds,
        now: this.outputPacing.now,
        setTimer: this.outputPacing.setTimer,
        clearTimer: this.outputPacing.clearTimer,
        onDrop: drop => this.onResponseAudioStats({
          type: 'paced_dropped',
          responseId: id,
          ...drop,
        }),
      })
      this.pacers.set(id, pacer)
    }
    return pacer
  }

  /**
   * Provider output can arrive far faster than it can be played. Queue it
   * behind a real-time clock here so the client never has to hold a long
   * backlog, which a barge-in would otherwise throw away unsent.
   */
  #sendPaced(id, event, seconds = 0) {
    const pacer = this.#pacerFor(id)
    if (!pacer) {
      this.send(event)
      return
    }
    if (pacer.closed) {
      // A cancelled response never resumes. Terminal bookkeeping still goes
      // out; queued audio must not leak to the client after the clear.
      if (seconds > 0) return
      this.send(event)
      return
    }
    pacer.push({ seconds, send: () => this.send(event) })
  }

  #dropPaced(id, reason) {
    const pacer = this.pacers.get(id)
    if (!pacer || pacer.closed) return null
    const result = pacer.flush(reason)
    if (result.droppedSeconds > 0) {
      this.onResponseAudioStats({
        type: 'paced_cancelled',
        responseId: id,
        ...result,
      })
    }
    return result
  }

  #audioDelta(event) {
    const id = realtimeResponseId(event)
    const context = this.#contextFor(event)
    if (context?.suppressed) return
    const responseTurnId = context.turnId || this.turns.turnId
    const sampleRate = Number(event.sampleRate)
      || this.getFrontend().provider.outputSampleRate
    const seconds = pcm16Seconds(event.delta, sampleRate)
    if (id) {
      context.hasAudio = true
      const stats = this.audioStats.get(id) || { seconds: 0, firstAt: 0, lastAt: 0 }
      stats.seconds += seconds
      if (!stats.firstAt) stats.firstAt = Date.now()
      stats.lastAt = Date.now()
      this.audioStats.set(id, stats)
      this.playbackTurns.set(id, responseTurnId)
      this.announcementWindow.queueAudio(id, {
        turnId: responseTurnId,
        origin: context.origin || 'model',
      })
    }
    this.#sendPaced(id, {
      type: GatewayServerEvent.AUDIO_DELTA,
      audio: event.delta,
      sampleRate,
      responseId: id,
      turnId: responseTurnId,
    }, seconds)
  }

  #audioTranscriptDelta(event) {
    const id = realtimeResponseId(event)
    const context = this.#contextFor(event)
    if (context.suppressed) return
    if (!context.playbackStarted) {
      context.pendingTranscripts.push({
        content: event.delta || '',
        final: false,
      })
      return
    }
    this.#emitTranscript({
      id,
      context,
      content: event.delta || '',
      final: false,
    })
  }

  #audioTranscriptDone(event) {
    const id = realtimeResponseId(event)
    const context = this.#contextFor(event)
    if (context.suppressed) return
    context.transcriptDone = true
    context.assistantTranscript = event.transcript || ''
    if (!context.playbackStarted) {
      context.pendingTranscripts.push({
        content: event.transcript || '',
        final: true,
      })
    } else {
      this.#emitTranscript({
        id,
        context,
        content: event.transcript || '',
        final: true,
      })
    }
    this.#finishContextIfComplete(id, context)
  }

  #textDelta(event) {
    const id = realtimeResponseId(event)
    const context = this.#contextFor(event)
    if (context.suppressed) return
    this.#emitTranscript({
      id,
      context,
      content: event.delta || '',
      final: false,
    })
  }

  #textDone(event) {
    const id = realtimeResponseId(event)
    const context = this.#contextFor(event)
    if (context.suppressed) return
    context.transcriptDone = true
    context.assistantTranscript = event.text || ''
    this.#emitTranscript({
      id,
      context,
      content: event.text || '',
      final: true,
    })
  }

  #responseDone(event) {
    const id = realtimeResponseId(event)
    const context = this.contexts.get(id)
    const terminalToolResponse = this.toolCalls.consumeTerminalToolResponse(id)
    const responseTurnId = context?.turnId || this.turns.turnId
    const responseStatus = event.response?.status
    const failed = ['failed', 'cancelled', 'incomplete'].includes(responseStatus)
    const sourceHasSpeech = Boolean(context?.hasAudio || context?.assistantTranscript?.trim())
    const suppressResponse = Boolean(failed || context?.suppressed)
    // A spoken task-acceptance receipt needs no second acknowledgement, but
    // inline tool results still need a summary after execution has finished.
    const suppressToolFollowUp = suppressResponse || (
      sourceHasSpeech && !this.toolCalls.requiresToolResultSummary?.(id)
    )
    const toolFollowUpPending = Boolean(
      context?.hasFunctionCall
      && !terminalToolResponse
      && !suppressToolFollowUp,
    )
    if (context) context.awaitsToolFollowUp = toolFollowUpPending
    // A failed/cancelled response has no playback left to schedule; drop the
    // queued burst before the terminal events go out.
    if (failed) this.#dropPaced(id, responseStatus)
    const guardDecision = evaluateResponseGuards({
      origin: context?.origin || 'model',
      hasFunctionCall: Boolean(context?.hasFunctionCall),
      failed,
      suppressed: Boolean(context?.suppressed),
      transcript: context?.assistantTranscript || '',
    })
    if (!context?.suppressed) {
      this.#sendPaced(id, {
        type: GatewayServerEvent.AUDIO_DONE,
        responseId: id,
        turnId: responseTurnId,
      })
      if (!context?.hasAudio && !toolFollowUpPending) {
        this.send({
          type: GatewayServerEvent.VOICE_STATE,
          state: 'idle',
          turnId: responseTurnId,
          origin: context?.origin || 'model',
        })
      }
    } else if (!context.hasAudio && responseTurnId === this.turns.committedTurnId && !this.turns.userSpeaking) {
      this.send({ type: GatewayServerEvent.VOICE_STATE, state: 'idle', turnId: responseTurnId })
    }
    if (context?.hasAudio && !failed) {
      context.responseDone = true
      this.#finishContextIfComplete(id, context)
    } else {
      const nonVoiceClient = this.getNonVoiceClient()
      const completedNonVoiceAnnouncement = (
        context?.origin === 'announcement'
        && nonVoiceClient
        && !failed
      )
      const completedNonVoiceTaskNotification = (
        context?.consumesTaskNotification
        && nonVoiceClient
        && !failed
      )
      if (
        context
        && !failed
        && (
          context.origin !== 'announcement'
          || completedNonVoiceAnnouncement
        )
      ) {
        this.#flushPendingTranscripts(id, context)
      }
      if (context?.origin === 'announcement') {
        if (completedNonVoiceAnnouncement) {
          this.announcements.confirmMany(contextTaskIds(context))
        } else {
          this.announcements.retryMany(contextTaskIds(context))
        }
      } else if (completedNonVoiceTaskNotification) {
        this.announcements.confirmMany(contextTaskIds(context))
      }
      this.contexts.delete(id)
      this.pacers.delete(id)
    }
    if (failed && id) {
      this.playbackTurns.delete(id)
      this.announcementWindow.finishPlayback(id, {
        awaitsToolFollowUp: false,
      })
    }
    this.announcementWindow.responseDone({
      turnId: responseTurnId,
      origin: context?.origin || 'model',
      hasAudio: Boolean(context?.hasAudio && !context?.playbackEnded),
      awaitsToolFollowUp: toolFollowUpPending,
      suppressed: Boolean(context?.suppressed) || terminalToolResponse,
      failed,
    })
    // Set the source response's waiting state first: a failed tool batch can
    // settle synchronously and must not have its release overwritten below.
    this.toolCalls.finishToolResponse(id, {
      suppressResponse,
      sourceHasSpeech,
    }).catch(error => this.send({
      type: GatewayServerEvent.ERROR,
      message: error.message,
    }))
    if (guardDecision) this.#requestGuardCorrection(guardDecision, context, responseTurnId)
    this.#flushAnnouncementsSoon()
    this.#reportAudioStats(id, responseStatus)
  }

  /**
   * How much audio one provider response produced and how fast it arrived.
   * A realtime factor far above 1 means the client would otherwise hold a
   * long backlog; the queued figure is what pacing is holding back right now.
   */
  #reportAudioStats(id, status) {
    const stats = this.audioStats.get(id)
    if (!stats) return
    this.audioStats.delete(id)
    const burstMs = stats.firstAt ? Math.max(1, Date.now() - stats.firstAt) : 0
    this.onResponseAudioStats({
      type: 'response',
      responseId: id,
      status: status || '',
      audioSeconds: Number(stats.seconds.toFixed(3)),
      burstMs,
      realtimeFactor: burstMs
        ? Number((stats.seconds / (burstMs / 1000)).toFixed(2))
        : 0,
      queuedSeconds: Number((this.pacers.get(id)?.queuedSeconds || 0).toFixed(3)),
    })
  }

  #requestGuardCorrection(decision, context, responseTurnId) {
    const frontend = this.getFrontend()
    if (
      !this.getOutputEnabled()
      || !frontend?.ready
      || !frontend.capabilities.perResponseInstructions
    ) return
    const generation = context?.turnGeneration
    const isCurrent = () => isResponseGuardTurnCurrent({
      sameFrontend: this.getFrontend() === frontend,
      outputEnabled: this.getOutputEnabled(),
      userSpeaking: this.turns.userSpeaking,
      responseTurnId,
      responseTurnGeneration: generation,
      committedTurnId: this.turns.committedTurnId,
      committedTurnGeneration: this.turns.committedTurnGeneration,
    })
    const turnKey = JSON.stringify([responseTurnId, generation])
    if (!isCurrent() || this.lastCorrectionTurn === turnKey) return
    // The budget belongs to the user turn, not a Response. A correction may
    // itself be invalid; it must never become a self-sustaining response loop.
    this.lastCorrectionTurn = turnKey
    frontend.ensureResponse({
      turnId: responseTurnId,
      turnGeneration: generation,
    }, {
      shouldCreate: isCurrent,
      response: { instructions: decision.instructions },
    }).catch(error => this.send({
      type: GatewayServerEvent.ERROR,
      message: error.message,
    }))
  }

  #emitTranscript({ id, context, content, final }) {
    const citations = final && String(content || '').trim()
      ? this.turnCitations?.consume(context.turnId) || []
      : []
    const invalidModelProtocol = Boolean(
      final
      && context.origin === 'model'
      && containsReservedProtocolEnvelope(content),
    )
    if (final && !invalidModelProtocol) {
      this.conversationSync.record({
        ownerId: this.ownerId,
        sessionId: this.sessionId,
        id: `voice:assistant:${id}`,
        role: 'assistant',
        content,
        source: context.origin === 'model'
          ? 'realtime-direct'
          : 'agent-presentation',
        ...(citations.length ? { citations } : {}),
        ...context,
      })
    }
    this.#sendPaced(id, {
      type: final
        ? GatewayServerEvent.TRANSCRIPT_FINAL
        : GatewayServerEvent.TRANSCRIPT_DELTA,
      role: 'assistant',
      content: content || '',
      responseId: id,
      ...(citations.length ? { citations } : {}),
      ...this.publicContext(context),
    })
  }

  #flushPendingTranscripts(id, context) {
    for (const transcript of context?.pendingTranscripts || []) {
      this.#emitTranscript({
        id,
        context,
        content: transcript.content,
        final: transcript.final,
      })
    }
    if (context) context.pendingTranscripts = []
  }

  #finishContextIfComplete(id, context) {
    if (
      context
      && context.playbackEnded
      && context.responseDone
      && context.transcriptDone
    ) {
      this.contexts.delete(id)
      this.pacers.delete(id)
    }
  }

  #scheduleContextCleanup(id, context) {
    const timer = setTimeout(() => {
      if (this.contexts.get(id) !== context) return
      this.contexts.delete(id)
      this.pacers.delete(id)
      this.audioStats.delete(id)
      this.playbackTurns.delete(id)
      this.announcementWindow.finishPlayback(id, {
        awaitsToolFollowUp: awaitsToolFollowUp(context),
      })
    }, this.responseContextCleanupMs)
    timer.unref?.()
  }

  #flushAnnouncementsSoon() {
    const timer = setTimeout(
      () => this.announcements.flush(),
      this.announcementQuietMs,
    )
    timer.unref?.()
  }

  startPlayback(id) {
    const context = this.contexts.get(id)
    if (context?.suppressed) return
    this.announcementWindow.startPlayback(id)
    const playbackTurnId = context?.turnId
      || this.playbackTurns.get(id)
      || this.turns.turnId
    this.send({
      type: GatewayServerEvent.VOICE_STATE,
      state: 'speaking',
      turnId: playbackTurnId,
      origin: context?.origin || 'model',
    })
    if (!context || context.playbackStarted) return
    context.playbackStarted = true
    if (confirmsTaskNotificationOnPlaybackStart(context)) {
      this.announcements.confirmMany(contextTaskIds(context))
    }
    this.#flushPendingTranscripts(id, context)
  }

  finishPlayback(id) {
    const playbackTurnId = this.playbackTurns.get(id) || this.turns.turnId
    const context = this.contexts.get(id)
    if (context?.suppressed) {
      this.playbackTurns.delete(id)
      return
    }
    this.announcementWindow.finishPlayback(id, {
      awaitsToolFollowUp: awaitsToolFollowUp(context),
    })
    this.playbackTurns.delete(id)
    if (context) {
      context.playbackEnded = true
      this.#finishContextIfComplete(id, context)
      if (this.contexts.get(id) === context) {
        this.#scheduleContextCleanup(id, context)
      }
    }
    const remainsProcessing = Boolean(
      awaitsToolFollowUp(context),
    )
    this.send({
      type: GatewayServerEvent.VOICE_STATE,
      state: this.turns.userSpeaking
        ? 'listening'
        : remainsProcessing
          ? 'processing'
          : 'idle',
      turnId: this.turns.userSpeaking ? this.turns.turnId : playbackTurnId,
      origin: context?.origin || 'model',
    })
    this.#flushAnnouncementsSoon()
  }

  failToolResponse({ responseId, turnId }) {
    const context = this.contexts.get(responseId)
    if (context) context.awaitsToolFollowUp = false
    const hasAudio = Boolean(context?.hasAudio && !context.playbackEnded)
    this.announcementWindow.responseDone({
      turnId,
      origin: context?.origin || 'model',
      hasAudio,
      failed: true,
    })
    if (!hasAudio && turnId === this.turns.turnId && !this.turns.userSpeaking) {
      this.send({
        type: GatewayServerEvent.VOICE_STATE,
        state: 'idle',
        turnId,
        origin: context?.origin || 'model',
      })
    }
    this.#flushAnnouncementsSoon()
  }

  clearPlayback({ reason = '' } = {}) {
    // A Gateway playback.clear is authoritative. Do not keep cleared audio
    // blocking results just because a client cancellation receipt is lost.
    for (const id of this.playbackTurns.keys()) {
      this.cancelPlayback(id, { reason })
    }
  }

  cancelPlayback(id, { reason = '' } = {}) {
    const context = this.contexts.get(id)
    if (context?.suppressed && context.playbackEnded) return
    // Whatever the provider already produced but the client has not heard is
    // discarded together with the playback it belongs to.
    this.#dropPaced(id, reason || 'cancelled')
    this.announcementWindow.finishPlayback(id, {
      awaitsToolFollowUp: false,
    })
    const playbackTurnId = this.playbackTurns.get(id) || this.turns.turnId
    this.playbackTurns.delete(id)
    if (context?.origin === 'announcement') {
      if (reason === 'user_interruption') {
        this.announcements.confirmMany(contextTaskIds(context))
      } else {
        this.announcements.retryMany(contextTaskIds(context))
      }
    }
    if (context?.playbackStarted && reason === 'user_interruption') {
      this.send({
        type: GatewayServerEvent.RESPONSE_INTERRUPTED,
        responseId: id,
        ...this.publicContext(context),
      })
    }
    if (context) {
      context.suppressed = true
      context.playbackEnded = true
      context.pendingTranscripts = []
      this.#scheduleContextCleanup(id, context)
    }
    this.send({
      type: GatewayServerEvent.VOICE_STATE,
      state: this.turns.userSpeaking ? 'listening' : 'idle',
      turnId: this.turns.userSpeaking ? this.turns.turnId : playbackTurnId,
      origin: context?.origin || 'model',
    })
    this.#flushAnnouncementsSoon()
  }

  cancelPermission(authorizationId) {
    for (const [responseId, context] of this.contexts) {
      if (
        context.origin === 'permission'
        && context.authorizationId === authorizationId
        && !context.suppressed
      ) {
        this.cancelPlayback(responseId, { reason: 'permission_resolved' })
      }
    }
  }

  failResponse(event) {
    const id = realtimeResponseId(event)
    const context = this.contexts.get(id)
    // The response never completed, so nothing it produced should still play.
    this.#dropPaced(id, 'failed')
    if (context?.origin === 'announcement') {
      this.send({ type: GatewayServerEvent.PLAYBACK_CLEAR })
      this.announcementWindow.finishPlayback(id)
      this.playbackTurns.delete(id)
      this.contexts.delete(id)
      this.pacers.delete(id)
      this.announcements.retryMany(contextTaskIds(context))
    } else {
      if (id && context?.hasAudio) {
        this.send({
          type: GatewayServerEvent.AUDIO_DONE,
          responseId: id,
          turnId: context.turnId || this.turns.turnId,
        })
        this.#scheduleContextCleanup(id, context)
      } else if (id) {
        this.contexts.delete(id)
        this.pacers.delete(id)
        this.playbackTurns.delete(id)
      }
      this.announcementWindow.responseDone({
        turnId: context?.turnId || this.turns.turnId,
        origin: context?.origin || 'model',
        hasAudio: Boolean(context?.hasAudio),
        awaitsToolFollowUp: false,
        failed: true,
      })
    }
    this.#flushAnnouncementsSoon()
  }

  clear() {
    for (const pacer of this.pacers.values()) pacer.flush('session_closed')
    this.pacers.clear()
    this.audioStats.clear()
    this.contexts.clear()
    this.playbackTurns.clear()
  }
}
