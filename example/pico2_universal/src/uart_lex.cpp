#include "uart_lex.hpp"

#include <cstring>

// Flush the accumulated literal token and return true if a token was produced.
static bool lexFlushLiteral(Lexer& lex) {
  if (lex.tokLen == 0) return false;
  lex.tokBuf[lex.tokLen] = '\0';

  Token& t = lex.pending;
  t = {};
  if (strcmp(lex.tokBuf, "true") == 0) {
    t.type = TokType::BOOL_TRUE;
  } else if (strcmp(lex.tokBuf, "false") == 0) {
    t.type = TokType::BOOL_FALSE;
  } else if (strcmp(lex.tokBuf, "null") == 0) {
    t.type = TokType::NULL_;
  } else {
    // Try integer
    t.type = TokType::INTEGER;
    int32_t val = 0;
    bool neg = false;
    int i = 0;
    if (lex.tokBuf[0] == '-') {
      neg = true;
      i = 1;
    }
    for (; i < lex.tokLen; i++) {
      char c = lex.tokBuf[i];
      if (c < '0' || c > '9') {
        t.type = TokType::NONE;
        break;
      }
      val = val * 10 + (c - '0');
    }
    t.intVal = neg ? -val : val;
  }
  lex.tokLen = 0;
  return t.type != TokType::NONE;
}

// Feed one character; returns true if a token is ready in lex.pending.
bool lexPush(Lexer& lex, char c) {
  lex.tokenReady = false;

  // Track CRLF for end-of-command detection
  if (c == '\r') {
    lex.crReceived = true;
  } else if (c == '\n' && lex.crReceived) {
    lex.crReceived = false;
    lex.lineReady = true;
    if (lex.state == LexState::IN_LITERAL) {
      return lexFlushLiteral(lex);
    }
    return false;
  } else {
    lex.crReceived = false;
  }

  switch (lex.state) {
    case LexState::IDLE:
      if (c == '"') {
        lex.state = LexState::IN_STRING;
        lex.tokLen = 0;
      } else if (c == '{') {
        lex.pending = {};
        lex.pending.type = TokType::BRACE_OPEN;
        return true;
      } else if (c == '}') {
        lex.pending = {};
        lex.pending.type = TokType::BRACE_CLOSE;
        return true;
      } else if (c == ':') {
        lex.pending = {};
        lex.pending.type = TokType::COLON;
        return true;
      } else if (c == ',') {
        lex.pending = {};
        lex.pending.type = TokType::COMMA;
        return true;
      } else if ((c >= '0' && c <= '9') || c == '-') {
        lex.state = LexState::IN_LITERAL;
        lex.tokLen = 0;
        lex.tokBuf[lex.tokLen++] = c;
      } else if (c == 't' || c == 'f' || c == 'n') {
        lex.state = LexState::IN_LITERAL;
        lex.tokLen = 0;
        lex.tokBuf[lex.tokLen++] = c;
      }
      break;

    case LexState::IN_STRING:
      if (c == '\\') {
        lex.state = LexState::IN_ESCAPE;
      } else if (c == '"') {
        lex.state = LexState::IDLE;
        lex.tokBuf[lex.tokLen] = '\0';
        lex.pending = {};
        lex.pending.type = TokType::STRING;
        memcpy(lex.pending.buf, lex.tokBuf,
               static_cast<size_t>(lex.tokLen + 1));
        lex.tokLen = 0;
        return true;
      } else {
        if (lex.tokLen < TOK_MAX_LEN) lex.tokBuf[lex.tokLen++] = c;
      }
      break;

    case LexState::IN_ESCAPE:
      if (lex.tokLen < TOK_MAX_LEN) {
        // Only handle basic escapes
        switch (c) {
          case '"':
          case '\\':
          case '/': lex.tokBuf[lex.tokLen++] = c; break;
          case 'n': lex.tokBuf[lex.tokLen++] = '\n'; break;
          case 'r': lex.tokBuf[lex.tokLen++] = '\r'; break;
          case 't': lex.tokBuf[lex.tokLen++] = '\t'; break;
          default: lex.tokBuf[lex.tokLen++] = c; break;
        }
      }
      lex.state = LexState::IN_STRING;
      break;

    case LexState::IN_LITERAL: {
      bool isLiteralChar = (c >= '0' && c <= '9') || c == '-' ||
                           (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
      if (isLiteralChar) {
        if (lex.tokLen < TOK_MAX_LEN) lex.tokBuf[lex.tokLen++] = c;
      } else {
        // End of literal — flush it; save the delimiter for next call to
        // avoid overwriting lex.pending with the delimiter token.
        lex.state = LexState::IDLE;
        bool had = lexFlushLiteral(lex);
        if (had) {
          lex.deferredChar = c;
          return true;
        }
        return lexPush(lex, c);
      }
      break;
    }
  }
  return false;
}
