//===- FuncConversions.cpp - Function conversions -------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "mlir/Dialect/Func/Transforms/FuncConversions.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Transforms/DialectConversion.h"

using namespace mlir;
using namespace mlir::func;

namespace {
/// Converts the operand and result types of the CallOp, used together with the
/// FuncOpSignatureConversion.
struct CallOpSignatureConversion
    : public OpInterfaceConversionPattern<CallOpInterface> {
  using OpInterfaceConversionPattern<
      CallOpInterface>::OpInterfaceConversionPattern;

  /// Attempt to match against code rooted at the specified operation,
  /// which is the same operation code as getRootKind().
  LogicalResult
  matchAndRewrite(CallOpInterface op, ArrayRef<Value> operands,
                  ConversionPatternRewriter &rewriter) const final {
    if (!typeConverter->isLegal(op)) {
      // Convert the original function results.
      SmallVector<Type, 1> convertedResults;
      if (failed(typeConverter->convertTypes(op->getResultTypes(),
                                             convertedResults)))
        return failure();

      // Substitute with the new result types from the corresponding FuncType
      // conversion.
      auto *newOp =
          op.clone(rewriter, op->getLoc(), convertedResults, operands);

      rewriter.replaceOp(op, newOp->getResults());
    }
    return success();
  }
};
} // namespace

void mlir::populateCallOpTypeConversionPattern(RewritePatternSet &patterns,
                                               TypeConverter &converter) {
  patterns.add<CallOpSignatureConversion>(converter, patterns.getContext());
}

namespace {
/// Only needed to support partial conversion of functions where this pattern
/// ensures that the branch operation arguments matches up with the succesor
/// block arguments.
class BranchOpInterfaceTypeConversion
    : public OpInterfaceConversionPattern<BranchOpInterface> {
public:
  using OpInterfaceConversionPattern<
      BranchOpInterface>::OpInterfaceConversionPattern;

  BranchOpInterfaceTypeConversion(
      TypeConverter &typeConverter, MLIRContext *ctx,
      function_ref<bool(BranchOpInterface, int)> shouldConvertBranchOperand)
      : OpInterfaceConversionPattern(typeConverter, ctx, /*benefit=*/1),
        shouldConvertBranchOperand(shouldConvertBranchOperand) {}

  LogicalResult
  matchAndRewrite(BranchOpInterface op, ArrayRef<Value> operands,
                  ConversionPatternRewriter &rewriter) const final {
    // For a branch operation, only some operands go to the target blocks, so
    // only rewrite those.
    SmallVector<Value, 4> newOperands(op->operand_begin(), op->operand_end());
    for (int succIdx = 0, succEnd = op->getBlock()->getNumSuccessors();
         succIdx < succEnd; ++succIdx) {
      OperandRange forwardedOperands =
          op.getSuccessorOperands(succIdx).getForwardedOperands();
      if (forwardedOperands.empty())
        continue;

      for (int idx = forwardedOperands.getBeginOperandIndex(),
               eidx = idx + forwardedOperands.size();
           idx < eidx; ++idx) {
        if (!shouldConvertBranchOperand || shouldConvertBranchOperand(op, idx))
          newOperands[idx] = operands[idx];
      }
    }
    rewriter.updateRootInPlace(
        op, [newOperands, op]() { op->setOperands(newOperands); });
    return success();
  }

private:
  function_ref<bool(BranchOpInterface, int)> shouldConvertBranchOperand;
};
} // namespace

namespace {
/// Only needed to support partial conversion of functions where this pattern
/// ensures that the branch operation arguments matches up with the succesor
/// block arguments.
class ReturnOpTypeConversion : public OpConversionPattern<ReturnOp> {
public:
  using OpConversionPattern<ReturnOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(ReturnOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const final {
    // For a return, all operands go to the results of the parent, so
    // rewrite them all.
    rewriter.updateRootInPlace(op,
                               [&] { op->setOperands(adaptor.getOperands()); });
    return success();
  }
};
} // namespace

void mlir::populateBranchOpInterfaceTypeConversionPattern(
    RewritePatternSet &patterns, TypeConverter &typeConverter,
    function_ref<bool(BranchOpInterface, int)> shouldConvertBranchOperand) {
  patterns.add<BranchOpInterfaceTypeConversion>(
      typeConverter, patterns.getContext(), shouldConvertBranchOperand);
}

bool mlir::isLegalForBranchOpInterfaceTypeConversionPattern(
    Operation *op, TypeConverter &converter) {
  // All successor operands of branch like operations must be rewritten.
  if (auto branchOp = dyn_cast<BranchOpInterface>(op)) {
    for (int p = 0, e = op->getBlock()->getNumSuccessors(); p < e; ++p) {
      auto successorOperands = branchOp.getSuccessorOperands(p);
      if (!converter.isLegal(
              successorOperands.getForwardedOperands().getTypes()))
        return false;
    }
    return true;
  }

  return false;
}

void mlir::populateReturnOpTypeConversionPattern(RewritePatternSet &patterns,
                                                 TypeConverter &typeConverter) {
  patterns.add<ReturnOpTypeConversion>(typeConverter, patterns.getContext());
}

bool mlir::isLegalForReturnOpTypeConversionPattern(Operation *op,
                                                   TypeConverter &converter,
                                                   bool returnOpAlwaysLegal) {
  // If this is a `return` and the user pass wants to convert/transform across
  // function boundaries, then `converter` is invoked to check whether the the
  // `return` op is legal.
  if (isa<ReturnOp>(op) && !returnOpAlwaysLegal)
    return converter.isLegal(op);

  // ReturnLike operations have to be legalized with their parent. For
  // return this is handled, for other ops they remain as is.
  return op->hasTrait<OpTrait::ReturnLike>();
}

bool mlir::isNotBranchOpInterfaceOrReturnLikeOp(Operation *op) {
  // If it is not a terminator, ignore it.
  if (!op->mightHaveTrait<OpTrait::IsTerminator>())
    return true;

  // If it is not the last operation in the block, also ignore it. We do
  // this to handle unknown operations, as well.
  Block *block = op->getBlock();
  if (!block || &block->back() != op)
    return true;

  // We don't want to handle terminators in nested regions, assume they are
  // always legal.
  if (!isa_and_nonnull<FuncOp>(op->getParentOp()))
    return true;

  return false;
}
