#include "main/lsp/requests/implementation.h"
#include "core/lsp/QueryResponse.h"
#include "main/lsp/LSPLoop.h"
#include "main/lsp/LSPQuery.h"
#include "main/lsp/json_types.h"

using namespace std;

namespace sorbet::realmain::lsp {

namespace {
struct MethodImplementationResults {
    vector<core::Loc> locations;
    unique_ptr<ResponseError> error;
};

unique_ptr<ResponseError> makeInvalidParamsError(string error) {
    return make_unique<ResponseError>((int)LSPErrorCodes::InvalidParams, error);
}

unique_ptr<ResponseError> makeInvalidRequestError(core::SymbolRef symbol, const core::GlobalState &gs) {
    auto errorMessage = fmt::format("Selected symbol `{}` is not a method or a reference of an abstract class. `Go To "
                                    "Implementation` request can't be applied",
                                    symbol.show(gs));
    return make_unique<ResponseError>((int)LSPErrorCodes::InvalidParams, errorMessage);
}

const MethodImplementationResults findMethodImplementations(const core::GlobalState &gs, core::MethodRef method) {
    MethodImplementationResults res;
    auto flags = method.data(gs)->flags;
    if (!flags.isAbstract && !flags.isOverridable) {
        res.error = makeInvalidRequestError(method, gs);
        return res;
    }

    vector<core::Loc> locations;
    auto owner = method.data(gs)->owner;
    auto includeOwner = false;
    // Scans whole symbol table. This is slow, and we might need to make this faster eventually.
    auto childClasses = getSubclassesSlow(gs, owner, includeOwner);
    auto methodName = method.data(gs)->name;
    for (const auto &childClass : childClasses) {
        auto methodImplementation = childClass.data(gs)->findMethod(gs, methodName);
        locations.push_back(methodImplementation.data(gs)->loc());
    }

    res.locations = locations;
    return res;
}

core::MethodRef findOverriddenMethod(const core::GlobalState &gs, const core::MethodRef method) {
    auto ownerClass = method.data(gs)->owner;

    for (auto mixin : ownerClass.data(gs)->mixins()) {
        return mixin.data(gs)->findMethod(gs, method.data(gs)->name);
    }
    return core::Symbols::noMethod();
}
} // namespace

ImplementationTask::ImplementationTask(const LSPConfiguration &config, MessageId id,
                                       unique_ptr<ImplementationParams> params)
    : LSPRequestTask(config, move(id), LSPMethod::TextDocumentImplementation), params(move(params)) {}

unique_ptr<ResponseMessage> ImplementationTask::runRequest(LSPTypecheckerDelegate &typechecker) {
    auto response = make_unique<ResponseMessage>("2.0", id, LSPMethod::TextDocumentImplementation);

    const core::GlobalState &gs = typechecker.state();
    auto queryResult = LSPQuery::byLoc(config, typechecker, params->textDocument->uri, *params->position,
                                       LSPMethod::TextDocumentImplementation);

    if (queryResult.error) {
        // An error happened while setting up the query.
        response->error = move(queryResult.error);
        return response;
    }

    vector<unique_ptr<Location>> result;
    if (queryResult.responses.empty()) {
        response->result = std::move(result);
        return response;
    }

    auto queryResponse = move(queryResult.responses[0]);
    if (auto def = queryResponse->isMethodDef()) {
        // User called "Go to Implementation" from the abstract function definition
        auto method = def->symbol;
        core::MethodRef overriddenMethod = method;
        if (method.data(gs)->flags.isOverride) {
            overriddenMethod = findOverriddenMethod(gs, method);
        }
        auto locationsOrError = findMethodImplementations(gs, overriddenMethod);

        if (locationsOrError.error != nullptr) {
            response->error = move(locationsOrError.error);
            return response;
        } else {
            for (const auto &location : locationsOrError.locations) {
                addLocIfExists(gs, result, location);
            }
        }
    } else if (auto constant = queryResponse->isConstant()) {
        // User called "Go to Implementation" from the abstract class reference
        auto classSymbol = constant->symbolBeforeDealias.dealias(gs).asClassOrModuleRef();

        if (!classSymbol.data(gs)->flags.isAbstract) {
            response->error = makeInvalidRequestError(classSymbol, gs);
            return response;
        }

        auto includeClassSymbol = false;
        auto childClasses = getSubclassesSlow(gs, classSymbol, includeClassSymbol);
        for (const auto &childClass : childClasses) {
            for (auto loc : childClass.data(gs)->locs()) {
                addLocIfExists(gs, result, loc);
            }
        }

    } else if (auto send = queryResponse->isSend()) {
        auto mainResponse = move(send->dispatchResult->main);

        // User called "Go to Implementation" from the abstract function call
        if (mainResponse.errors.size() != 0) {
            response->error = makeInvalidParamsError("Failed to fetch implementations");
            return response;
        }

        auto calledMethod = mainResponse.method;
        auto overriddenMethod = calledMethod;
        if (calledMethod.data(gs)->flags.isOverride) {
            overriddenMethod = findOverriddenMethod(gs, overriddenMethod);
        }

        auto locationsOrError = findMethodImplementations(gs, overriddenMethod);

        if (locationsOrError.error != nullptr) {
            response->error = move(locationsOrError.error);
            return response;
        } else {
            for (const auto &location : locationsOrError.locations) {
                addLocIfExists(gs, result, location);
            }
        }
    }

    response->result = move(result);
    return response;
}
} // namespace sorbet::realmain::lsp
