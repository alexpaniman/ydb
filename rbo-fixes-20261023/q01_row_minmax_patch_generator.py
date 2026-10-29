"""Build an artifact-only Q01 amendment; do not mutate a production checkout."""
import difflib
import subprocess

SOURCE_COMMIT = 'fcf071da437b3602ae63869477e74da02a166f92'
CPP = 'ydb/core/kqp/opt/rbo/physical_conversion/kqp_rbo_physical_window_builder.cpp'
HEADER = CPP.removesuffix('.cpp') + '.h'


ELIGIBILITY = r'''
TString TPhysicalWindowBuilder::FrameOrdinalName() const {
    return Names.GetTemporaryName("__kqp_win_frame_ordinal_");
}

bool TPhysicalWindowBuilder::CanUseRowMinMax(TInfoUnitId column) const {
    const auto& frame = Window.GetFrame();
    auto bounded = [](EWindowFrameBound bound) {
        return bound == EWindowFrameBound::Preceding || bound == EWindowFrameBound::CurrentRow || bound == EWindowFrameBound::Following;
    };
    if (frame.Type != EWindowFrameType::Rows || !bounded(frame.BeginKind) || !bounded(frame.EndKind)) {
        return false;
    }
    const auto& func = *Window.GetWindowFuncs().Find(column);
    if (func.Kind != EWindowFuncKind::Aggregate ||
        (func.Function != "min" && func.Function != "max") || func.Arguments.Items().size() != 1) {
        return false;
    }
    const auto* type = InputItemType(func.Arguments.Items().front());
    if (type->GetKind() == ETypeAnnotationKind::Optional) {
        type = type->Cast<TOptionalExprType>()->GetItemType();
    }
    if (type->GetKind() != ETypeAnnotationKind::Data) {
        return false;
    }
    // These selectors compare a total scalar order and return an input value.
    // No arithmetic or comparison of opaque/composite/floating values is moved.
    switch (type->Cast<TDataExprType>()->GetSlot()) {
        case NUdf::EDataSlot::Bool:
        case NUdf::EDataSlot::Int8:
        case NUdf::EDataSlot::Uint8:
        case NUdf::EDataSlot::Int16:
        case NUdf::EDataSlot::Uint16:
        case NUdf::EDataSlot::Int32:
        case NUdf::EDataSlot::Uint32:
        case NUdf::EDataSlot::Int64:
        case NUdf::EDataSlot::Uint64:
        case NUdf::EDataSlot::Decimal:
        case NUdf::EDataSlot::Interval:
        case NUdf::EDataSlot::Interval64:
        case NUdf::EDataSlot::Date:
        case NUdf::EDataSlot::Datetime:
        case NUdf::EDataSlot::Timestamp:
        case NUdf::EDataSlot::Date32:
        case NUdf::EDataSlot::Datetime64:
        case NUdf::EDataSlot::Timestamp64:
            return true;
        default:
            return false;
    }
}

bool TPhysicalWindowBuilder::HasRowMinMax() const {
    for (const auto output : Functions) {
        if (CanUseRowMinMax(output)) {
            return true;
        }
    }
    return false;
}
'''


BUILDER = r'''
TExprNode::TPtr TPhysicalWindowBuilder::BuildRowMinMaxOutputs(TExprNode::TPtr collected, TExprNode::TPtr queue) const {
    auto nth = [&](TExprNode::TPtr tuple, ui32 index) {
        return Ctx.NewCallable(Pos, "Nth", {tuple, Ctx.NewAtom(Pos, ToString(index))});
    };
    auto makeLambda = [&](bool update) {
        auto row = Ctx.NewArgument(Pos, "minmax_row");
        TExprNode::TListType args{row};
        TExprNode::TPtr previous;
        if (update) {
            auto state = Ctx.NewArgument(Pos, "minmax_previous");
            args.push_back(state);
            previous = nth(state, 1);
        }
        auto frameRows = Ctx.NewCallable(Pos, "WinFrame", {
            queue, BuildUint64(0),
            Ctx.NewCallable(Pos, "Bool", {Ctx.NewAtom(Pos, "false")}),
            Ctx.NewCallable(Pos, "Bool", {Ctx.NewAtom(Pos, "false")}),
            Ctx.NewCallable(Pos, "Bool", {Ctx.NewAtom(Pos, "false")}),
            Ctx.NewCallable(Pos, "DependsOn", {row})});
        auto firstRow = Ctx.NewCallable(Pos, "Head", {frameRows});
        auto lastRow = Ctx.ChangeChild(*frameRows, 4,
            Ctx.NewCallable(Pos, "Bool", {Ctx.NewAtom(Pos, "true")}));
        auto previousEnd = update ? Member(previous, FrameOrdinalName()) : BuildUint64(0);
        auto last = Ctx.NewArgument(Pos, "minmax_last");
        auto lastOrdinal = Ctx.NewCallable(Pos, "IfPresent", {lastRow,
            Ctx.NewLambda(Pos, Ctx.NewArguments(Pos, {last}), Member(last, FrameOrdinalName())), previousEnd});

        // A fixed ROWS right bound advances by at most one row. Only the first
        // nonempty frame needs an initial scan; clipped tails add no new row.
        auto entering = frameRows;
        if (update) {
            auto emptyRows = Ctx.NewCallable(Pos, "List", {
                Ctx.NewCallable(Pos, "ListType", {BuildOutputRowType(/*framePrefixSums=*/true)})});
            auto enteringLast = Ctx.NewArgument(Pos, "minmax_entering_last");
            auto newLast = Ctx.NewCallable(Pos, "If", {
                Ctx.NewCallable(Pos, ">", {Member(enteringLast, FrameOrdinalName()), previousEnd}),
                Ctx.NewCallable(Pos, "AsList", {enteringLast}), emptyRows});
            auto singleNewRow = Ctx.NewCallable(Pos, "IfPresent", {lastRow,
                Ctx.NewLambda(Pos, Ctx.NewArguments(Pos, {enteringLast}), newLast), emptyRows});
            entering = Ctx.NewCallable(Pos, "If", {
                Ctx.NewCallable(Pos, "==", {previousEnd, BuildUint64(0)}), frameRows, singleNewRow});
        }

        TVector<std::pair<TString, TExprNode::TPtr>> states{{FrameOrdinalName(), lastOrdinal}};
        auto outputRow = row;
        for (ui32 f = 0; f < Functions.size(); ++f) {
            if (!CanUseRowMinMax(Functions[f])) {
                continue;
            }
            const auto& func = *Window.GetWindowFuncs().Find(Functions[f]);
            const auto argument = func.Arguments.Items().front();
            const auto* argumentType = InputItemType(argument);
            const bool optional = argumentType->GetKind() == ETypeAnnotationKind::Optional;
            const TTypeAnnotationNode* valueType = optional ? argumentType : Ctx.MakeType<TOptionalExprType>(argumentType);
            const auto* entryType = Ctx.MakeType<TTupleExprType>(TTypeAnnotationNode::TListType{
                Ctx.MakeType<TDataExprType>(NUdf::EDataSlot::Uint64), valueType, valueType});
            auto empty = Ctx.NewCallable(Pos, "List", {ExpandType(Pos, *Ctx.MakeType<TListExprType>(entryType), Ctx)});
            auto absent = Ctx.NewCallable(Pos, "Nothing", {ExpandType(Pos, *valueType, Ctx)});
            auto combine = [&](TExprNode::TPtr left, TExprNode::TPtr right) {
                return Ctx.NewCallable(Pos, func.Function == "min" ? "AggrMin" : "AggrMax", {left, right});
            };
            auto summary = [&](TExprNode::TPtr stack) {
                auto head = Ctx.NewArgument(Pos, "minmax_head");
                return Ctx.NewCallable(Pos, "IfPresent", {Ctx.NewCallable(Pos, "Head", {stack}),
                    Ctx.NewLambda(Pos, Ctx.NewArguments(Pos, {head}), nth(head, 2)), absent});
            };
            auto first = Ctx.NewArgument(Pos, "minmax_first");
            auto trim = [&](TExprNode::TPtr front) {
                auto entry = Ctx.NewArgument(Pos, "minmax_expired");
                auto expired = Ctx.NewLambda(Pos, Ctx.NewArguments(Pos, {entry}),
                    Ctx.NewCallable(Pos, "<", {nth(entry, 0), Member(first, FrameOrdinalName())}));
                // Front is a direct list built only by Prepend during transfer.
                // Afterwards it is only skipped; this retains O(1) list slices.
                auto count = Ctx.NewCallable(Pos, "Length", {Ctx.NewCallable(Pos, "TakeWhile", {front, expired})});
                return Ctx.NewCallable(Pos, "Skip", {front, count});
            };
            auto old = update ? Member(previous, AccumulatorName(f)) : Ctx.NewList(Pos, {empty, empty});
            auto oldRear = nth(old, 1);
            auto trimmed = trim(nth(old, 0));
            auto hasFront = Ctx.NewCallable(Pos, "HasItems", {trimmed});

            auto entry = Ctx.NewArgument(Pos, "minmax_transfer_entry");
            auto front = Ctx.NewArgument(Pos, "minmax_transfer_front");
            auto transferredEntry = Ctx.NewList(Pos, {nth(entry, 0), nth(entry, 1), combine(nth(entry, 1), summary(front))});
            auto transferLambda = Ctx.NewLambda(Pos, Ctx.NewArguments(Pos, {entry, front}),
                Ctx.NewCallable(Pos, "Prepend", {transferredEntry, front}));
            auto transferred = Ctx.NewCallable(Pos, "Fold", {oldRear, empty, transferLambda});
            auto nextFront = Ctx.NewCallable(Pos, "If", {hasFront, trimmed, trim(transferred)});
            auto retainedRear = Ctx.NewCallable(Pos, "If", {hasFront, oldRear, empty});

            auto addedRow = Ctx.NewArgument(Pos, "minmax_added_row");
            auto rear = Ctx.NewArgument(Pos, "minmax_rear");
            auto value = MakeOptional(Member(addedRow, Names.Get(argument)), optional);
            auto addedEntry = Ctx.NewList(Pos, {Member(addedRow, FrameOrdinalName()), value, combine(summary(rear), value)});
            // Rear is only prepended until transfer, never skipped and then
            // prepended. HolderFactory can therefore preserve Normal capacity.
            auto pushLambda = Ctx.NewLambda(Pos, Ctx.NewArguments(Pos, {addedRow, rear}),
                Ctx.NewCallable(Pos, "Prepend", {addedEntry, rear}));
            auto nextRear = Ctx.NewCallable(Pos, "Fold", {entering, retainedRear, pushLambda});
            auto pair = Ctx.NewCallable(Pos, "IfPresent", {firstRow,
                Ctx.NewLambda(Pos, Ctx.NewArguments(Pos, {first}), Ctx.NewList(Pos, {nextFront, nextRear})),
                Ctx.NewList(Pos, {empty, empty})});
            states.emplace_back(AccumulatorName(f), pair);
            auto valueForFrame = combine(summary(nth(pair, 0)), summary(nth(pair, 1)));
            outputRow = Ctx.NewCallable(Pos, "ReplaceMember", {outputRow, Ctx.NewAtom(Pos, Names.Get(Functions[f])),
                BuildResultFromAccumulator(Functions[f], valueForFrame)});
        }
        return Ctx.NewLambda(Pos, Ctx.NewArguments(Pos, std::move(args)), Ctx.NewList(Pos, {outputRow, BuildStruct(states)}));
    };
    auto chained = Ctx.NewCallable(Pos, "Chain1Map", {collected, makeLambda(false), makeLambda(true)});
    auto result = Ctx.NewArgument(Pos, "minmax_output");
    return Ctx.NewCallable(Pos, "OrderedMap", {chained,
        Ctx.NewLambda(Pos, Ctx.NewArguments(Pos, {result}), nth(result, 0))});
}
'''


def _replace_once(text, before, after):
    if text.count(before) != 1:
        raise ValueError('Expected unique production source anchor')
    return text.replace(before, after, 1)


def render_files(repository):
    def source(path):
        return subprocess.check_output(['git', 'show', SOURCE_COMMIT + ':' + path],
                                       cwd=repository, text=True)
    cpp, header = source(CPP), source(HEADER)
    cpp = _replace_once(cpp, 'TString TPhysicalWindowBuilder::PositionName', ELIGIBILITY + '\nTString TPhysicalWindowBuilder::PositionName')
    cpp = _replace_once(cpp, '    if (framePrefixSums) {\n        for (ui32 f = 0;',
                        '    if (framePrefixSums) {\n        if (HasRowMinMax()) {\n            fields.push_back(Ctx.MakeType<TItemExprType>(FrameOrdinalName(), Ctx.MakeType<TDataExprType>(NUdf::EDataSlot::Uint64)));\n        }\n        for (ui32 f = 0;')
    cpp = _replace_once(cpp, '    if (NeedsPeerKey) {\n        for (ui32 k = 0;',
                        '    if (framePrefixSums && HasRowMinMax()) {\n        auto ordinal = update ? Ctx.NewCallable(Pos, "Inc", {Member(previousState, FrameOrdinalName())}) : BuildUint64(1);\n        stateMembers.emplace_back(FrameOrdinalName(), ordinal);\n        outputMembers.emplace_back(FrameOrdinalName(), ordinal);\n    }\n\n    if (NeedsPeerKey) {\n        for (ui32 k = 0;')
    cpp = _replace_once(cpp, '(func.Function == "count" || CanUsePrefixSum(funcs[f]))))',
                        '(func.Function == "count" || CanUsePrefixSum(funcs[f]) || CanUseRowMinMax(funcs[f]))))')
    cpp = _replace_once(cpp, 'TExprNode::TPtr TPhysicalWindowBuilder::BuildFrameFold', BUILDER + '\nTExprNode::TPtr TPhysicalWindowBuilder::BuildFrameFold')
    cpp = _replace_once(cpp, '    auto collected = BuildCollector(BuildChainOutputs(wideFlow, /*framePrefixSums=*/true), queue, bounds, ascending);',
                        '    auto collected = BuildCollector(BuildChainOutputs(wideFlow, /*framePrefixSums=*/true), queue, bounds, ascending);\n    if (HasRowMinMax()) {\n        collected = BuildRowMinMaxOutputs(collected, queue);\n    }')
    anchor = '        if (CanUsePrefixSum(funcs[f])) {\n            auto first = Ctx.NewArgument(Pos, "first_row");'
    cpp = _replace_once(cpp, anchor, '        if (CanUseRowMinMax(funcs[f])) {\n            members.emplace_back(name, Member(rowArg, name));\n            continue;\n        }\n\n' + anchor)
    header = _replace_once(header, '    bool CanUsePrefixSum(TInfoUnitId column) const;',
                           '    bool CanUsePrefixSum(TInfoUnitId column) const;\n    TString FrameOrdinalName() const;\n    bool CanUseRowMinMax(TInfoUnitId column) const;\n    bool HasRowMinMax() const;\n    TExprNode::TPtr BuildRowMinMaxOutputs(TExprNode::TPtr collected, TExprNode::TPtr queue) const;')
    return {CPP: (source(CPP), cpp), HEADER: (source(HEADER), header)}


def render_patch(repository):
    return ''.join('diff --git a/' + path + ' b/' + path + '\n' +
                   ''.join(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
                                                fromfile='a/' + path, tofile='b/' + path))
                   for path, (before, after) in render_files(repository).items())
