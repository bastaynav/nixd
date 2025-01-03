/// \file
/// \brief Implementation of [Document Symbol].
/// [Document Symbol]:
/// https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/#textDocument_documentSymbol

#include "CheckReturn.h"
#include "Convert.h"

#include "nixd/Controller/Controller.h"

#include <boost/asio/post.hpp>
#include <llvm/ADT/StringRef.h>
#include <lspserver/Protocol.h>
#include <nixf/Basic/Nodes/Attrs.h>
#include <nixf/Basic/Nodes/Lambda.h>
#include <nixf/Sema/VariableLookup.h>

#include <string>

using namespace nixd;
using namespace lspserver;
using namespace nixf;

namespace {

std::string getLambdaName(const ExprLambda &Lambda) {
  if (!Lambda.arg() || !Lambda.arg()->id())
    return "(anonymous lambda)";
  return Lambda.arg()->id()->name();
}

lspserver::Range getLambdaSelectionRage(llvm::StringRef Src,
                                        const ExprLambda &Lambda) {
  if (!Lambda.arg()) {
    return toLSPRange(Src, Lambda.range());
  }

  if (!Lambda.arg()->id()) {
    assert(Lambda.arg()->formals());
    return toLSPRange(Src, Lambda.arg()->formals()->range());
  }
  return toLSPRange(Src, Lambda.arg()->id()->range());
}

lspserver::Range getAttrRange(llvm::StringRef Src, const Attribute &Attr) {
  auto LCur = toLSPPosition(Src, Attr.key().lCur());
  if (Attr.value())
    return {LCur, toLSPPosition(Src, Attr.value()->rCur())};
  return {LCur, toLSPPosition(Src, Attr.key().rCur())};
}

/// Make variable's entry rich.
void richVar(const ExprVar &Var, DocumentSymbol &Sym,
             const VariableLookupAnalysis &VLA) {
  if (Var.id().name() == "true" || Var.id().name() == "false") {
    Sym.kind = SymbolKind::Boolean;
    Sym.detail = "builtin boolean";
    return;
  }

  if (Var.id().name() == "null") {
    Sym.kind = SymbolKind::Null;
    Sym.detail = "null";
    return;
  }

  auto Result = VLA.query(Var);
  using ResultKind = VariableLookupAnalysis::LookupResultKind;
  if (Result.Kind == ResultKind::Defined)
    Sym.kind = SymbolKind::Constant;
  else if (Result.Kind == ResultKind::FromWith)
    Sym.kind = SymbolKind::Variable;
  else {
    Sym.deprecated = true;
    return;
  }

  if (Result.Def->isBuiltin())
    Sym.kind = SymbolKind::Event;
}

/// Collect document symbol on AST.
void collect(const Node *AST, std::vector<DocumentSymbol> &Symbols,
             const VariableLookupAnalysis &VLA, llvm::StringRef Src) {
  if (!AST)
    return;
  switch (AST->kind()) {

  case Node::NK_ExprString: {
    const auto &Str = static_cast<const ExprString &>(*AST);
    DocumentSymbol Sym{
        .name = Str.isLiteral() ? Str.literal() : "(dynamic string)",
        .detail = "string",
        .kind = SymbolKind::String,
        .deprecated = false,
        .range = toLSPRange(Src, Str.range()),
        .selectionRange = toLSPRange(Src, Str.range()),
        .children = {},
    };
    Symbols.emplace_back(std::move(Sym));
    break;
  }
  case Node::NK_ExprInt: {
    const auto &Int = static_cast<const ExprInt &>(*AST);
    DocumentSymbol Sym{
        .name = std::to_string(Int.value()),
        .detail = "integer",
        .kind = SymbolKind::Number,
        .deprecated = false,
        .range = toLSPRange(Src, Int.range()),
        .selectionRange = toLSPRange(Src, Int.range()),
        .children = {},
    };
    Symbols.emplace_back(std::move(Sym));
    break;
  }
  case Node::NK_ExprFloat: {
    const auto &Float = static_cast<const ExprFloat &>(*AST);
    DocumentSymbol Sym{
        .name = std::to_string(Float.value()),
        .detail = "float",
        .kind = SymbolKind::Number,
        .deprecated = false,
        .range = toLSPRange(Src, Float.range()),
        .selectionRange = toLSPRange(Src, Float.range()),
        .children = {},
    };
    Symbols.emplace_back(std::move(Sym));
    break;
  }
  case Node::NK_AttrName: {
    const auto &AN = static_cast<const AttrName &>(*AST);
    DocumentSymbol Sym{
        .name = AN.isStatic() ? AN.staticName() : "(dynamic attribute name)",
        .detail = "attribute name",
        .kind = SymbolKind::Property,
        .deprecated = false,
        .range = toLSPRange(Src, AN.range()),
        .selectionRange = toLSPRange(Src, AN.range()),
        .children = {},
    };
    Symbols.emplace_back(std::move(Sym));
    break;
  }
  case Node::NK_ExprVar: {
    const auto &Var = static_cast<const ExprVar &>(*AST);
    DocumentSymbol Sym{
        .name = Var.id().name(),
        .detail = "identifier",
        .kind = SymbolKind::Variable,
        .deprecated = false,
        .range = toLSPRange(Src, Var.range()),
        .selectionRange = toLSPRange(Src, Var.range()),
        .children = {},
    };
    richVar(Var, Sym, VLA);
    Symbols.emplace_back(std::move(Sym));
    break;
  }
  case Node::NK_ExprLambda: {
    std::vector<DocumentSymbol> Children;
    const auto &Lambda = static_cast<const ExprLambda &>(*AST);
    collect(Lambda.body(), Children, VLA, Src);
    DocumentSymbol Sym{
        .name = getLambdaName(Lambda),
        .detail = "lambda",
        .kind = SymbolKind::Function,
        .deprecated = false,
        .range = toLSPRange(Src, Lambda.range()),
        .selectionRange = getLambdaSelectionRage(Src, Lambda),
        .children = std::move(Children),
    };
    Symbols.emplace_back(std::move(Sym));
    break;
  }
  case Node::NK_ExprList: {
    std::vector<DocumentSymbol> Children;
    const auto &List = static_cast<const ExprList &>(*AST);
    for (const Node *Ch : AST->children())
      collect(Ch, Children, VLA, Src);

    DocumentSymbol Sym{
        .name = "{anonymous}",
        .detail = "list",
        .kind = SymbolKind::Array,
        .deprecated = false,
        .range = toLSPRange(Src, List.range()),
        .selectionRange = toLSPRange(Src, List.range()),
        .children = std::move(Children),
    };
    Symbols.emplace_back(std::move(Sym));
    break;
  }
  case Node::NK_ExprAttrs: {
    const SemaAttrs &SA = static_cast<const ExprAttrs &>(*AST).sema();
    for (const auto &[Name, Attr] : SA.staticAttrs()) {
      if (!Attr.value())
        continue;
      std::vector<DocumentSymbol> Children;
      collect(Attr.value(), Children, VLA, Src);
      DocumentSymbol Sym{
          .name = Name,
          .detail = "attribute",
          .kind = SymbolKind::Field,
          .deprecated = false,
          .range = getAttrRange(Src, Attr),
          .selectionRange = toLSPRange(Src, Attr.key().range()),
          .children = std::move(Children),
      };
      Symbols.emplace_back(std::move(Sym));
    }
    for (const nixf::Attribute &Attr : SA.dynamicAttrs()) {
      std::vector<DocumentSymbol> Children;
      collect(Attr.value(), Children, VLA, Src);
      DocumentSymbol Sym{
          .name = "${dynamic attribute}",
          .detail = "attribute",
          .kind = SymbolKind::Field,
          .deprecated = false,
          .range = getAttrRange(Src, Attr),
          .selectionRange = toLSPRange(Src, Attr.key().range()),
          .children = std::move(Children),
      };
      Symbols.emplace_back(std::move(Sym));
    }
    break;
  }
  default:
    // Trivial dispatch. Treat these symbol as same as this level.
    for (const Node *Ch : AST->children())
      collect(Ch, Symbols, VLA, Src);
    break;
  }
}

} // namespace

void Controller::onDocumentSymbol(const DocumentSymbolParams &Params,
                                  Callback<std::vector<DocumentSymbol>> Reply) {
  using CheckTy = std::vector<DocumentSymbol>;
  auto Action = [Reply = std::move(Reply), URI = Params.textDocument.uri,
                 this]() mutable {
    return Reply([&]() -> llvm::Expected<CheckTy> {
      const auto TU = CheckDefault(getTU(URI.file().str()));
      const auto AST = CheckDefault(getAST(*TU));
      auto Symbols = std::vector<DocumentSymbol>();
      collect(AST.get(), Symbols, *TU->variableLookup(), TU->src());
      return Symbols;
    }());
  };
  boost::asio::post(Pool, std::move(Action));
}
