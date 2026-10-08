#include "xFields.h"
#include <string.h>

namespace idisplay {

// ---- xYesNoField ----

xFieldResult xYesNoField::onKey(xKey key, xKeyAction action) {
    if (action != xKeyAction::Pressed) return xFieldResult::None;
    switch (key) {
    case xKey::Up:
    case xKey::Down:
        value_ = !value_;
        return xFieldResult::Changed;
    case xKey::Enter: return xFieldResult::Done;
    case xKey::Back:  return xFieldResult::Cancelled;
    default:          return xFieldResult::None;
    }
}

void xYesNoField::render(iTextSurface& s) const {
    s.print(value_ ? yes_ : no_);
}

// ---- xChoiceField ----

bool xChoiceField::add(const char* text) {
    if (count_ >= kMaxChoices) return false;
    texts_[count_++] = text;
    return true;
}

xFieldResult xChoiceField::onKey(xKey key, xKeyAction action) {
    if (action != xKeyAction::Pressed) return xFieldResult::None;
    switch (key) {
    case xKey::Up:
        if (sel_ > 0) --sel_;
        return xFieldResult::Changed;
    case xKey::Down:
        if (sel_ + 1 < count_) ++sel_;
        return xFieldResult::Changed;
    case xKey::Enter: return xFieldResult::Done;
    case xKey::Back:  return xFieldResult::Cancelled;
    default:          return xFieldResult::None;
    }
}

void xChoiceField::render(iTextSurface& s) const {
    if (sel_ < count_) s.print(texts_[sel_]);
}

// ---- xTextField ----

const char xTextField::kCharsetUpper[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 ";
const char xTextField::kCharsetAlnum[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 -_.";
const char xTextField::kCharsetPrintable[] =
    " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~";

// Does not read buf: xTextFieldN passes its own storage before that is
// constructed. begin() reads it.
xTextField::xTextField(char* buf, uint8_t capacity, const char* charset)
    : buf_(buf), cap_(capacity), charset_(charset),
      setLen_((uint8_t)strlen(charset)) {}

void xTextField::begin() {
    len_ = 0;
    if (cap_ == 0) return;
    while (len_ + 1 < cap_ && buf_[len_] != '\0') ++len_;
    buf_[len_] = '\0';
    pos_ = 0;
    loadCandidate();
}

void xTextField::loadCandidate() {
    if (pos_ >= len_) {
        cand_ = endIndex();
        return;
    }
    const char* hit = strchr(charset_, buf_[pos_]);
    cand_ = (hit != nullptr && buf_[pos_] != '\0') ? (uint8_t)(hit - charset_) : kKeep;
}

char xTextField::glyphFor(uint8_t c) const {
    if (c == kKeep) return buf_[pos_];
    if (c == delIndex()) return delGlyph_;
    if (c == endIndex()) return endGlyph_;
    return charset_[c];
}

xFieldResult xTextField::onKey(xKey key, xKeyAction action) {
    if (action != xKeyAction::Pressed || cap_ == 0) return xFieldResult::None;
    const uint8_t options = (uint8_t)(setLen_ + 2);
    const bool full = pos_ + 1 >= cap_;
    switch (key) {
    case xKey::Down:
    case xKey::Up:
        if (full) {
            // No room for another character: only the two commands.
            cand_ = cand_ == endIndex() ? delIndex() : endIndex();
            return xFieldResult::Changed;
        }
        break;
    default:
        break;
    }
    switch (key) {
    case xKey::Down:
        cand_ = cand_ == kKeep ? 0 : (uint8_t)((cand_ + 1) % options);
        return xFieldResult::Changed;
    case xKey::Up:
        cand_ = cand_ == kKeep ? (uint8_t)(options - 1) : (uint8_t)((cand_ + options - 1) % options);
        return xFieldResult::Changed;
    case xKey::Back:
        return xFieldResult::Cancelled;
    case xKey::Enter:
        if (cand_ == endIndex()) {
            len_ = pos_;
            buf_[len_] = '\0';
            return xFieldResult::Done;
        }
        if (cand_ == delIndex()) {
            if (pos_ == 0) return xFieldResult::None;
            // Remove the character before the cursor, NUL included in the move.
            memmove(&buf_[pos_ - 1], &buf_[pos_], (size_t)(len_ - pos_ + 1));
            --len_;
            --pos_;
            loadCandidate();
            return xFieldResult::Changed;
        }
        if (full) return xFieldResult::None;
        buf_[pos_] = glyphFor(cand_);
        if (pos_ == len_) {
            ++len_;
            buf_[len_] = '\0';
        }
        ++pos_;
        loadCandidate();
        return xFieldResult::Changed;
    default:
        return xFieldResult::None;
    }
}

void xTextField::render(iTextSurface& s) const {
    const uint8_t startCol = s.cursorCol();
    if (startCol >= s.cols()) return;
    const uint8_t width = (uint8_t)(s.cols() - startCol);
    const uint8_t first = pos_ >= width ? (uint8_t)(pos_ - width + 1) : 0;
    const bool wasInverse = s.inverse();
    for (uint8_t i = first; i < (uint8_t)(first + width); ++i) {
        if (i == pos_) {
            s.setInverse(true);
            s.putChar(glyphFor(cand_));
            s.setInverse(wasInverse);
        } else if (i < len_) {
            s.putChar(buf_[i]);
        } else {
            break;
        }
    }
}

} // namespace idisplay
