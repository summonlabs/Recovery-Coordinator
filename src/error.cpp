#include "recovery/error.hpp"

namespace recovery {

std::string_view to_string(ErrorClass value) {
    switch (value) {
        case ErrorClass::InvalidArgument:
            return "invalid_argument";
        case ErrorClass::PreconditionFailed:
            return "precondition_failed";
        case ErrorClass::IllegalTransition:
            return "illegal_transition";
        case ErrorClass::NotFound:
            return "not_found";
        case ErrorClass::IdentityConflict:
            return "identity_conflict";
        case ErrorClass::Persistence:
            return "persistence";
        case ErrorClass::CorruptState:
            return "corrupt_state";
        case ErrorClass::StaleAuthority:
            return "stale_authority";
        case ErrorClass::Replayed:
            return "replayed";
        case ErrorClass::Overflow:
            return "overflow";
        case ErrorClass::Unsupported:
            return "unsupported";
        case ErrorClass::Contended:
            return "contended";
        case ErrorClass::LimitExceeded:
            return "limit_exceeded";
        case ErrorClass::OperationFailed:
            return "operation_failed";
        case ErrorClass::Internal:
            return "internal";
    }
    return "internal";
}

std::string Error::describe() const {
    std::string out;
    out.append(to_string(cls_));
    out.append("/");
    out.append(code_);
    if (!message_.empty()) {
        out.append(": ");
        out.append(message_);
    }
    return out;
}

namespace errors {

Error invalid_argument(std::string code, std::string message) {
    return Error{ErrorClass::InvalidArgument, std::move(code), std::move(message)};
}
Error precondition_failed(std::string code, std::string message) {
    return Error{ErrorClass::PreconditionFailed, std::move(code), std::move(message)};
}
Error illegal_transition(std::string code, std::string message) {
    return Error{ErrorClass::IllegalTransition, std::move(code), std::move(message)};
}
Error not_found(std::string code, std::string message) {
    return Error{ErrorClass::NotFound, std::move(code), std::move(message)};
}
Error identity_conflict(std::string code, std::string message) {
    return Error{ErrorClass::IdentityConflict, std::move(code), std::move(message)};
}
Error persistence(std::string code, std::string message) {
    return Error{ErrorClass::Persistence, std::move(code), std::move(message)};
}
Error corrupt_state(std::string code, std::string message) {
    return Error{ErrorClass::CorruptState, std::move(code), std::move(message)};
}
Error stale_authority(std::string code, std::string message) {
    return Error{ErrorClass::StaleAuthority, std::move(code), std::move(message)};
}
Error replayed(std::string code, std::string message) {
    return Error{ErrorClass::Replayed, std::move(code), std::move(message)};
}
Error overflow(std::string code, std::string message) {
    return Error{ErrorClass::Overflow, std::move(code), std::move(message)};
}
Error unsupported(std::string code, std::string message) {
    return Error{ErrorClass::Unsupported, std::move(code), std::move(message)};
}
Error contended(std::string code, std::string message) {
    return Error{ErrorClass::Contended, std::move(code), std::move(message)};
}
Error limit_exceeded(std::string code, std::string message) {
    return Error{ErrorClass::LimitExceeded, std::move(code), std::move(message)};
}
Error operation_failed(std::string code, std::string message) {
    return Error{ErrorClass::OperationFailed, std::move(code), std::move(message)};
}
Error internal(std::string code, std::string message) {
    return Error{ErrorClass::Internal, std::move(code), std::move(message)};
}

}  // namespace errors

}  // namespace recovery
