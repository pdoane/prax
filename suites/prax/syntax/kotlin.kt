// A repository of notes over a suspend API, with a sealed result.
package com.example.notes

import kotlinx.coroutines.delay
import kotlinx.coroutines.runBlocking

data class Note(val id: Long, val title: String, val body: String = "", val pinned: Boolean = false)

sealed interface Result<out T> {
    data class Ok<T>(val value: T) : Result<T>
    data class Failed(val reason: String) : Result<Nothing>
}

interface NoteApi {
    suspend fun fetch(): List<Note>
}

class NoteRepository(private val api: NoteApi) {
    private val cache = mutableMapOf<Long, Note>()

    suspend fun refresh(): Result<Int> = try {
        val notes = api.fetch()
        cache.clear()
        notes.associateByTo(cache) { it.id }
        Result.Ok(notes.size)
    } catch (e: Exception) {
        Result.Failed(e.message ?: "unknown error")
    }

    fun pinnedFirst(): List<Note> =
        cache.values.sortedWith(compareByDescending<Note> { it.pinned }.thenBy { it.title })

    companion object {
        const val MAX_TITLE = 120
    }
}

fun main() = runBlocking {
    val repository = NoteRepository(object : NoteApi {
        override suspend fun fetch(): List<Note> {
            delay(10L)
            return listOf(Note(1, "Groceries"), Note(2, "Ideas", pinned = true))
        }
    })
    when (val result = repository.refresh()) {
        is Result.Ok -> println("loaded ${result.value} notes:\n${repository.pinnedFirst().joinToString { it.title }}")
        is Result.Failed -> println("failed: ${result.reason}")
    }
}
