#pragma once

#include <cstdint>

static constexpr int TOK_MAX_LEN = 64;

enum class TokType {
  NONE,
  BRACE_OPEN,
  BRACE_CLOSE,
  COLON,
  COMMA,
  STRING,
  INTEGER,
  BOOL_TRUE,
  BOOL_FALSE,
  NULL_,
};

struct Token {
  TokType type = TokType::NONE;
  char buf[TOK_MAX_LEN + 1] = {};
  int32_t intVal = 0;
};

enum class LexState {
  IDLE,
  IN_STRING,
  IN_ESCAPE,
  IN_LITERAL,
};

struct Lexer {
  LexState state = LexState::IDLE;
  char tokBuf[TOK_MAX_LEN + 1] = {};
  int tokLen = 0;
  bool tokenReady = false;
  Token pending;
  bool crReceived = false;
  bool lineReady = false;  // CRLF received
  char deferredChar = 0;   // delimiter deferred from end of literal token

  void reset() {
    state = LexState::IDLE;
    tokLen = 0;
    tokenReady = false;
    crReceived = false;
    lineReady = false;
    deferredChar = 0;
  }
};

// Feed one character; returns true if a token is ready in lex.pending.
bool lexPush(Lexer& lex, char c);
