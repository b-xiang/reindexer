#include <sstream>

#include "core/cjson/cjsonencoder.h"
#include "core/cjson/jsonencoder.h"
#include "core/index/index.h"
#include "core/namespace.h"
#include "nsselecter.h"
#include "tools/logger.h"
#include "tools/stringstools.h"

using std::chrono::duration_cast;
using std::chrono::high_resolution_clock;
using std::chrono::microseconds;
using std::next;
using std::shared_ptr;
using std::string;
using std::stringstream;

// Number of sorted queries to namespace after last updated, to call very expensive buildSortOrders, to do futher queries fast
// If number of queries was less, than kBuildSortOrdersHitCount, then slow post process sort (applyGeneralSort) is
const int kBuildSortOrdersHitCount = 5;

namespace reindexer {
#define TIMEPOINT(n)                                  \
	std::chrono::high_resolution_clock::time_point n; \
	if (enableTiming) n = high_resolution_clock::now()
void NsSelecter::operator()(QueryResults &result, SelectCtx &ctx) {
	CollateOpts collateOpts;
	Index *sortIndex = nullptr;
	bool unorderedIndexSort = false;
	bool forcedSort = !ctx.query.forcedSortOrder.empty();

	if (ns_->queriesLogLevel_ > ctx.query.debugLevel) {
		const_cast<Query *>(&ctx.query)->debugLevel = ns_->queriesLogLevel_;
	}

	bool enableTiming = ctx.query.debugLevel >= LogInfo;

	TIMEPOINT(tmStart);

	bool needCalcTotal = ctx.query.calcTotal == ModeAccurateTotal;
	bool needPutCachedTotal = false;

	if (ctx.query.calcTotal == ModeCachedTotal) {
		auto cached = ns_->queryCache_->Get({ctx.query});
		if (cached.key && cached.val.total_count >= 0) {
			result.totalCount = cached.val.total_count;
			logPrintf(LogTrace, "[*] using value from cache: %d\t namespace: %s\n", result.totalCount, ns_->name_.c_str());
		} else {
			needPutCachedTotal = (cached.key != nullptr);
			logPrintf(LogTrace, "[*] value for cache will be calculated by query. namespace: %s\n", ns_->name_.c_str());
			needCalcTotal = true;
		}
	}

	const QueryEntries *whereEntries = &ctx.query.entries;
	QueryEntries tmpWhereEntries(ctx.skipIndexesLookup ? QueryEntries() : lookupQueryIndexes(ctx.query.entries));
	if (!ctx.skipIndexesLookup) {
		whereEntries = &tmpWhereEntries;
	}
	bool containsFullText = containsFullTextIndexes(*whereEntries);

	if (!ctx.skipIndexesLookup) {
		if (!containsFullText) substituteCompositeIndexes(tmpWhereEntries);
		updateCompositeIndexesValues(tmpWhereEntries);
	} else {
		// TODO refactor const cast!
		for (const QueryEntry &entry : *whereEntries) {
			if (entry.idxNo < ns_->payloadType_->NumFields()) {
				KeyValueType keyType = KeyValueEmpty;
				if (entry.idxNo == IndexValueType::SetByJsonPath) {
					keyType = getQueryEntryIndexType(entry);
				} else {
					keyType = ns_->indexes_[entry.idxNo]->KeyType();
				}
				if (keyType != KeyValueEmpty) {
					for (auto &key : entry.values) const_cast<KeyValue &>(key).convert(keyType);
				}
			}
		}
	}
	// DO NOT use deducted sort order in the following cases:
	// - query contains explicity specified sort order
	// - query contains FullText query.
	bool disableOptimizeSortOrder = !ctx.query.sortBy.empty() || ctx.preResult;

	auto sortBy = (containsFullText || disableOptimizeSortOrder) ? ctx.query.sortBy : getOptimalSortOrder(*whereEntries);
	int sortByIdxNo = sortBy.empty() ? -1 : ns_->getIndexByName(sortBy);

	if (ctx.preResult) {
		switch (ctx.preResult->mode) {
			case SelectCtx::PreResult::ModeBuild:
				ctx.preResult->sortBy = sortBy;
				break;
			case SelectCtx::PreResult::ModeIdSet:
			case SelectCtx::PreResult::ModeIterators:
				sortBy = ctx.preResult->sortBy;
				break;
			default:
				abort();
		}
	}

	// Check if commit needed
	bool needSortOrders = !sortBy.empty() && (ns_->sortedQueriesCount_ > kBuildSortOrdersHitCount || ctx.preResult || ctx.joinedSelectors);

	if (!whereEntries->empty() || needSortOrders) {
		FieldsSet indexesForCommit;
		for (const QueryEntry &entry : *whereEntries) {
			if (entry.idxNo != IndexValueType::SetByJsonPath) {
				indexesForCommit.push_back(entry.idxNo);
			}
		}
		if (sortByIdxNo >= 0) indexesForCommit.push_back(sortByIdxNo);
		for (int i = ns_->indexes_.firstCompositePos(); i < ns_->indexes_.totalSize(); i++) {
			if (indexesForCommit.contains(ns_->indexes_[i]->Fields())) indexesForCommit.push_back(i);
		}
		ns_->commit(Namespace::NSCommitContext(*ns_, CommitContext::MakeIdsets | (needSortOrders ? CommitContext::MakeSortOrders : 0),
											   &indexesForCommit),
					ctx.lockUpgrader);
	}

	if (sortByIdxNo >= 0) {
		// Query is sorted. Search for sort index
		sortIndex = ns_->indexes_[sortByIdxNo].get();
		if (sortIndex && sortIndex->IsOrdered()) ns_->sortedQueriesCount_++;
		if ((sortIndex && !sortIndex->IsOrdered()) || containsFullText || !ns_->sortOrdersBuilt_) {
			ctx.isForceAll = true;
			unorderedIndexSort = true;
			collateOpts = sortIndex->Opts().collateOpts_;
			sortIndex = nullptr;
		}
	}

	// Add preresults with common conditions of join Queres
	RawQueryResult qres;
	if (ctx.preResult && ctx.preResult->mode == SelectCtx::PreResult::ModeIdSet) {
		SelectKeyResult res;
		res.push_back(SingleSelectKeyResult(&ctx.preResult->ids));
		static string pr = "-preresult";
		qres.push_back(SelectIterator(res, OpAnd, false, pr));
	} else if (ctx.preResult && ctx.preResult->mode == SelectCtx::PreResult::ModeIterators) {
		for (auto &it : ctx.preResult->iterators) {
			qres.push_back(it);
		}
	}
	// Prepare data for select function
	if (ctx.functions) {
		fnc_ = ctx.functions->AddNamespace(ctx.query, *ns_, containsFullText);
	}
	TIMEPOINT(tm1);

	selectWhere(*whereEntries, qres, sortIndex ? sortIndex->SortId() : 0, containsFullText);

	TIMEPOINT(tm2);

	if (ctx.preResult && ctx.preResult->mode == SelectCtx::PreResult::ModeBuild) {
		// Building pre result for next joins
		int maxIters = 0;
		for (auto &it : qres) {
			maxIters = std::max(maxIters, it.GetMaxIterations());
		}

		// Return preResult as QueryIterators if:
		// 1. We have 1 QueryIterator with not more, than 2 idsets, just put it to preResults
		// 2. We have > QueryIterator which expects more than 10000 iterations.
		if ((qres.size() == 1 && qres[0].size() < 3) || maxIters >= 10000) {
			for (auto &it : qres) {
				ctx.preResult->iterators.push_back(it);
			}
			if (ctx.query.debugLevel >= LogInfo) {
				logPrintf(LogInfo, "%s", ctx.query.Dump().c_str());
				logPrintf(LogInfo, "Built prePresult (expected %d iterations) with %d iterators", maxIters, qres.size());
			}

			ctx.preResult->mode = SelectCtx::PreResult::ModeIterators;
			return;
		}
		// Build preResult as single IdSet
	}

	bool haveComparators = false, haveScan = false, haveIdsets = false;
	bool reverse = ctx.query.sortDirDesc && sortIndex && !containsFullText;

	for (auto &r : qres)
		if (r.comparators_.size())
			haveComparators = true;
		else
			haveIdsets = true;

	if (qres.empty() || (!containsFullText && (!haveIdsets || qres[0].op == OpNot))) {
		// special case - no condition or first result have comparator or not
		SelectKeyResult res;
		res.push_back(SingleSelectKeyResult(0, IdType(sortIndex ? sortIndex->SortOrders().size() : ns_->items_.size())));
		qres.insert(qres.begin(), SelectIterator(res, OpAnd, false, "-scan", true));
		haveScan = sortIndex && !forcedSort ? false : true;
	}

	// Get maximum iterations count, for right calculation comparators costs
	int iters = INT_MAX;
	for (auto r = qres.begin(); r != qres.end(); ++r) {
		int cur = r->GetMaxIterations();
		if (!r->comparators_.size() && cur && cur < iters) iters = cur;
	}

	// Sort by cost
	std::sort(qres.begin(), qres.end(),
			  [iters](const SelectIterator &i1, const SelectIterator &i2) { return i1.Cost(iters) < i2.Cost(iters); });

	// Check NOT or comparator must not be 1st
	for (auto r = qres.begin(); r != qres.end(); ++r) {
		if (r->op != OpNot && !r->comparators_.size()) {
			if (r != qres.begin()) {
				// if NOT found in 1-st position swap it with 1-st non NOT
				auto tmp = *r;
				*r = *qres.begin();
				*qres.begin() = tmp;
			}
			break;
		}
	}

	// Rewing all results iterators
	for (auto &r : qres) r.Start(reverse);

	// Let iterators choose most effecive algorith
	assert(qres.size());
	for (auto r = qres.begin() + 1; r != qres.end(); r++) r->SetExpectMaxIterations(iters);

	result.addNSContext(ns_->payloadType_, ns_->tagsMatcher_, JsonPrintFilter(ns_->tagsMatcher_, ctx.query.selectFilter_));

	TIMEPOINT(tm3);
	LoopCtx lctx(ctx);
	lctx.sortIndex = sortIndex;
	lctx.qres = &qres;
	lctx.ftIndex = containsFullText;
	lctx.calcTotal = needCalcTotal;
	result.haveProcent = containsFullText;
	if (reverse && haveComparators && haveScan) selectLoop<true, true, true>(lctx, result);
	if (!reverse && haveComparators && haveScan) selectLoop<false, true, true>(lctx, result);
	if (reverse && !haveComparators && haveScan) selectLoop<true, false, true>(lctx, result);
	if (!reverse && !haveComparators && haveScan) selectLoop<false, false, true>(lctx, result);
	if (reverse && haveComparators && !haveScan) selectLoop<true, true, false>(lctx, result);
	if (!reverse && haveComparators && !haveScan) selectLoop<false, true, false>(lctx, result);
	if (reverse && !haveComparators && !haveScan) selectLoop<true, false, false>(lctx, result);
	if (!reverse && !haveComparators && !haveScan) selectLoop<false, false, false>(lctx, result);

	TIMEPOINT(tm4);

	if (ctx.query.debugLevel >= LogInfo) {
		int count = (ctx.preResult && ctx.preResult->mode == SelectCtx::PreResult::ModeBuild) ? ctx.preResult->ids.size() : result.Count();
		logPrintf(LogInfo, "%s", ctx.query.Dump().c_str());
		logPrintf(LogInfo,
				  "Got %d items in %d µs [prepare %d µs, select %d µs, postprocess "
				  "%d µs loop %d µs], sortindex %s",
				  count, int(duration_cast<microseconds>(tm4 - tmStart).count()), int(duration_cast<microseconds>(tm1 - tmStart).count()),
				  int(duration_cast<microseconds>(tm2 - tm1).count()), int(duration_cast<microseconds>(tm3 - tm2).count()),
				  int(duration_cast<microseconds>(tm4 - tm3).count()), sortIndex ? sortIndex->Name().c_str() : "-");
		if (ctx.query.debugLevel >= LogTrace) {
			for (auto &r : qres)
				logPrintf(LogInfo, "%s: %d idsets, %d comparators, cost %g, matched %d", r.name.c_str(), r.size(), r.comparators_.size(),
						  r.Cost(iters), r.GetMatchedCount());
			if (ctx.joinedSelectors) {
				for (auto &js : *ctx.joinedSelectors) {
					if (js.type == JoinType::LeftJoin || js.type == JoinType::Merge) {
						logPrintf(LogInfo, "%s %s: called %d\n", Query::JoinTypeName(js.type), js.ns.c_str(), js.called);
					} else {
						logPrintf(LogInfo, "%s %s: called %d, matched %d\n", Query::JoinTypeName(js.type), js.ns.c_str(), js.called,
								  js.matched);
					}
				}
			}

			result.Dump();
		}
	}

	if (unorderedIndexSort) {
		applyGeneralSort(result.Items(), ctx, sortBy, collateOpts);
	}

	if (!ctx.query.forcedSortOrder.empty()) {
		applyCustomSort(result.Items(), ctx);
	}

	if (unorderedIndexSort || ctx.isForceAll) {
		setLimitsAndOffset(result.Items(), ctx);
	}

	if (needPutCachedTotal) {
		logPrintf(LogTrace, "[*] put totalCount value into query cache: %d\t namespace: %s\n", result.totalCount, ns_->name_.c_str());
		ns_->queryCache_->Put({ctx.query}, {static_cast<size_t>(result.totalCount)});
	}
	if (ctx.preResult && ctx.preResult->mode == SelectCtx::PreResult::ModeBuild) {
		ctx.preResult->mode = SelectCtx::PreResult::ModeIdSet;
		if (ctx.query.debugLevel >= LogInfo) {
			logPrintf(LogInfo, "Built idset prePresult with %d ids", ctx.preResult->ids.size());
		}
	}
}

void NsSelecter::applyCustomSort(ItemRefVector &queryResult, const SelectCtx &ctx) {
	if (ctx.query.mergeQueries_.size() > 1) throw Error(errLogic, "Force sort could not be applied to 'merged' queries.");

	auto payloadType = ns_->payloadType_;
	const string &fieldName = ctx.query.sortBy;

	int idx = ns_->getIndexByName(fieldName);

	if (ns_->indexes_[idx]->Opts().IsArray()) throw Error(errQueryExec, "This type of sorting cannot be applied to a field of array type.");

	KeyValues keyValues = ctx.query.forcedSortOrder;  // make a copy of forcedSortOrder', may be mark it as 'mutable'?
	ItemRefVector::difference_type cost = 0;
	KeyValueType fieldType = ns_->indexes_[idx]->KeyType();

	if (idx < payloadType->NumFields()) {
		// implementation for regular indexes
		fast_hash_map<KeyValue, ItemRefVector::difference_type> sortMap;
		for (auto &value : keyValues) {
			value.convert(fieldType);
			sortMap.insert({value, cost});
			cost++;
		}

		KeyRefs keyRefs;
		auto sortEnd =
			std::stable_partition(queryResult.begin(), queryResult.end(), [&sortMap, &payloadType, idx, &keyRefs](ItemRef &itemRef) {
				ConstPayload(payloadType, itemRef.value).Get(idx, keyRefs);
				return !keyRefs.empty() && (sortMap.find(keyRefs[0]) != sortMap.end());
			});

		KeyRefs firstItemValue;
		KeyRefs secondItemValue;
		std::sort(queryResult.begin(), sortEnd,
				  [&sortMap, &payloadType, idx, &firstItemValue, &secondItemValue](const ItemRef &lhs, const ItemRef &rhs) {
					  ConstPayload(payloadType, lhs.value).Get(idx, firstItemValue);
					  assertf(!firstItemValue.empty(), "Item lost in query results%s", "");
					  assertf(sortMap.find(firstItemValue[0]) != sortMap.end(), "Item not found in 'sortMap'%s", "");

					  ConstPayload(payloadType, rhs.value).Get(idx, secondItemValue);
					  assertf(sortMap.find(secondItemValue[0]) != sortMap.end(), "Item not found in 'sortMap'%s", "");
					  assertf(!secondItemValue.empty(), "Item lost in query results%s", "");

					  return sortMap.find(firstItemValue[0])->second < sortMap.find(secondItemValue[0])->second;
				  });
	} else {
		// implementation for composite indexes
		FieldsSet fields = ns_->indexes_[idx]->Fields();

		unordered_payload_map<ItemRefVector::difference_type> sortMap(0, hash_composite(payloadType, fields),
																	  equal_composite(payloadType, fields));

		for (auto &value : keyValues) {
			value.convertToComposite(payloadType, fields);
			sortMap.insert({static_cast<const PayloadValue &>(value), cost});
			cost++;
		}

		auto sortEnd = std::stable_partition(queryResult.begin(), queryResult.end(),
											 [&sortMap](ItemRef &itemRef) { return (sortMap.find(itemRef.value) != sortMap.end()); });

		std::sort(queryResult.begin(), sortEnd, [&sortMap](const ItemRef &lhs, const ItemRef &rhs) {
			return sortMap.find(lhs.value)->second < sortMap.find(rhs.value)->second;
		});
	}
}

void NsSelecter::applyGeneralSort(ItemRefVector &queryResult, const SelectCtx &ctx, const string &fieldName,
								  const CollateOpts &collateOpts) {
	if (ctx.query.mergeQueries_.size() > 1) throw Error(errLogic, "Sorting cannot be applied to merged queries.");

	auto &payloadType = ns_->payloadType_;
	bool sortAsc = !ctx.query.sortDirDesc;

	int fieldIdx = ns_->getIndexByName(fieldName);

	if (ns_->indexes_[fieldIdx]->Opts().IsArray()) throw Error(errQueryExec, "Sorting cannot be applied to an array field.");

	FieldsSet fields;

	if (fieldIdx >= payloadType->NumFields()) {
		fields = ns_->indexes_[fieldIdx]->Fields();
	} else {
		fields.push_back(fieldIdx);
	}

	int limit = std::min(ctx.query.count + ctx.query.start, queryResult.size());

	std::partial_sort(queryResult.begin(), queryResult.begin() + limit, queryResult.end(),
					  [&payloadType, &fields, sortAsc, collateOpts](const ItemRef &lhs, const ItemRef &rhs) {
						  bool comparisonResult = false;
						  if (sortAsc) {
							  comparisonResult = ConstPayload(payloadType, lhs.value).Compare(rhs.value, fields, collateOpts) < 0;
						  } else {
							  comparisonResult = ConstPayload(payloadType, lhs.value).Compare(rhs.value, fields, collateOpts) > 0;
						  }
						  return comparisonResult;
					  });
}

void NsSelecter::setLimitsAndOffset(ItemRefVector &queryResult, const SelectCtx &ctx) {
	unsigned offset = ctx.query.start;
	unsigned limit = ctx.query.count;
	unsigned totalRows = queryResult.size();

	if (offset > 0) {
		auto end = offset < totalRows ? queryResult.begin() + offset : queryResult.end();
		queryResult.erase(queryResult.begin(), end);
	}

	if (queryResult.size() > limit) {
		queryResult.erase(queryResult.begin() + limit, queryResult.end());
	}
}

bool NsSelecter::containsFullTextIndexes(const QueryEntries &entries) {
	bool result = false;
	for (const QueryEntry &entry : entries) {
		if ((entry.idxNo != IndexValueType::SetByJsonPath) && isFullText(ns_->indexes_[entry.idxNo]->Type())) {
			result = true;
			break;
		}
	}
	return result;
}

QueryEntries NsSelecter::lookupQueryIndexes(const QueryEntries &entries) {
	int iidx[maxIndexes];
	for (auto &i : iidx) i = -1;

	QueryEntries ret;
	for (auto entry = entries.begin(); entry != entries.end(); entry++) {
		QueryEntry currentEntry = *entry;
		if (currentEntry.idxNo == IndexValueType::NotSet) {
			if (!ns_->getIndexByName(currentEntry.index, currentEntry.idxNo)) {
				currentEntry.idxNo = IndexValueType::SetByJsonPath;
			}
		}
		bool byJsonPath = (currentEntry.idxNo == IndexValueType::SetByJsonPath);
		if (!byJsonPath && (currentEntry.idxNo < ns_->payloadType_->NumFields())) {
			for (KeyValue &key : currentEntry.values) key.convert(ns_->indexes_[currentEntry.idxNo]->KeyType());
		}

		// try merge entries with AND opetator
		auto nextEntry = entry;
		nextEntry++;
		if (!byJsonPath && (currentEntry.op == OpAnd) && (nextEntry == entries.end() || nextEntry->op == OpAnd)) {
			if (iidx[currentEntry.idxNo] >= 0 && !ns_->indexes_[currentEntry.idxNo]->Opts().IsArray()) {
				if (mergeQueryEntries(&ret[iidx[currentEntry.idxNo]], &currentEntry)) continue;
			} else {
				iidx[currentEntry.idxNo] = ret.size();
			}
		}
		ret.push_back(std::move(currentEntry));
	};
	return ret;
}

void NsSelecter::selectWhere(const QueryEntries &entries, RawQueryResult &result, unsigned sortId, bool is_ft) {
	bool fullText = false;
	for (const QueryEntry &qe : entries) {
		TagsPath tagsPath;
		SelectKeyResults selectResults;
		bool sparseIndex = false;
		bool byJsonPath = (qe.idxNo == IndexValueType::SetByJsonPath);
		if (byJsonPath) {
			KeyValueType keyType = qe.values.empty() ? KeyValueEmpty : qe.values[0].Type();
			tagsPath = ns_->tagsMatcher_.path2tag(qe.index);

			FieldsSet fields;
			fields.push_back(tagsPath);

			SelectKeyResult comparisonResult;
			comparisonResult.comparators_.push_back(
				Comparator(qe.condition, keyType, qe.values, false, ns_->payloadType_, fields, nullptr, CollateOpts()));
			selectResults.push_back(comparisonResult);
		} else {
			auto &index = ns_->indexes_[qe.idxNo];
			fullText = isFullText(index->Type());
			sparseIndex = index->Opts().IsSparse();

			Index::ResultType type = Index::Optimal;
			if (is_ft && qe.distinct) throw Error(errQueryExec, "distinct and full text - can't do it");
			if (is_ft)
				type = Index::ForceComparator;
			else if (qe.distinct)
				type = Index::ForceIdset;

			auto ctx = fnc_ ? fnc_->CreateCtx(qe.idxNo) : BaseFunctionCtx::Ptr{};
			if (ctx && ctx->type == BaseFunctionCtx::kFtCtx) ft_ctx_ = reindexer::reinterpret_pointer_cast<FtCtx>(ctx);

			if (index->Opts().GetCollateMode() == CollateUTF8 || fullText)
				for (auto &key : qe.values) key.EnsureUTF8();

			selectResults = index->SelectKey(qe.values, qe.condition, sortId, type, ctx);
		}
		for (auto res : selectResults) {
			switch (qe.op) {
				case OpOr:
					if (!result.size()) throw Error(errQueryExec, "OR operator in first condition");
					if (byJsonPath || sparseIndex) {
						result.back().Append(res);
					} else {
						result.back().AppendAndBind(res, ns_->payloadType_, qe.idxNo);
					}
					result.back().distinct |= qe.distinct;
					result.back().name += " OR " + qe.index;
					break;
				case OpNot:
				case OpAnd:
					result.push_back(SelectIterator(res, qe.op, qe.distinct, qe.index, fullText));
					if (!byJsonPath && !sparseIndex) result.back().Bind(ns_->payloadType_, qe.idxNo);
					break;
				default:
					throw Error(errQueryExec, "Unknown operator (code %d) in condition", qe.op);
			}
			if (fullText) {
				result.back().SetUnsorted();
			}
		}
	}
}

template <bool reverse, bool haveComparators, bool haveScan>
void NsSelecter::selectLoop(LoopCtx &ctx, QueryResults &result) {
	unsigned start = 0;
	unsigned count = UINT_MAX;
	SelectCtx &sctx = ctx.sctx;

	if (!sctx.isForceAll) {
		start = sctx.query.start;
		count = sctx.query.count;
	}
	auto aggregators = getAggregators(sctx.query);
	// do not calc total by loop, if we have only 1 condition with 1 idset
	bool calcTotal = ctx.calcTotal && (ctx.qres->size() > 1 || haveComparators || (*ctx.qres)[0].size() > 1);

	// reserve queryresults, if we have only 1 condition with 1 idset
	if (ctx.qres->size() == 1 && (*ctx.qres)[0].size() == 1) {
		unsigned reserve = std::min(unsigned(ctx.qres->at(0).GetMaxIterations()), count);
        result.Items().reserve(reserve);
	}

	bool finish = (count == 0) && !sctx.reqMatchedOnceFlag && !calcTotal;

	bool haveInnerJoin = false;
	if (sctx.joinedSelectors) {
		for (size_t i = 0; i < sctx.joinedSelectors->size(); i++) {
			auto &joinedSelector = sctx.joinedSelectors->at(i);
			haveInnerJoin |= (joinedSelector.type == JoinType::InnerJoin || joinedSelector.type == JoinType::OrInnerJoin);
		}
	}

	// TODO: nested conditions support. Like (A  OR B OR C) AND (X OR Z)
	auto &first = *ctx.qres->begin();
	IdType val = first.Val();
	assert(!ctx.sortIndex || ctx.sortIndex->IsOrdered());
	while (first.Next(val) && !finish) {
		val = first.Val();
		IdType realVal = val;

		if (haveScan && ns_->items_[realVal].IsFree()) continue;
		if (haveComparators && ctx.sortIndex) {
			assert(ctx.sortIndex->SortOrders().size() > static_cast<size_t>(val));
			realVal = ctx.sortIndex->SortOrders()[val];
		}

		bool found = true;
		for (auto cur = ctx.qres->begin() + 1; cur != ctx.qres->end(); cur++) {
			assert(static_cast<size_t>(realVal) < ns_->items_.size());
			PayloadValue &itemPayloadValue(ns_->items_[realVal]);
			assert(itemPayloadValue.Ptr() != nullptr);
			if (!haveComparators || !cur->TryCompare(itemPayloadValue, realVal)) {
				while (((reverse && cur->Val() > val) || (!reverse && cur->Val() < val)) && cur->Next(val)) {
				};

				if (cur->End()) {
					finish = true;
					found = false;
				} else if ((reverse && cur->Val() < val) || (!reverse && cur->Val() > val)) {
					found = false;
				}
			}
			bool isNot = cur->op == OpNot;
			if ((isNot && found) || (!isNot && !found)) {
				found = false;
				for (; cur != ctx.qres->end(); cur++) {
					if (cur->comparators_.size() || cur->op == OpNot || cur->End()) continue;
					if (reverse && cur->Val() < val) val = cur->Val() + 1;
					if (!reverse && cur->Val() > val) val = cur->Val() - 1;
				}
				break;
			} else if (isNot && !found) {
				found = true;
				finish = false;
			}
		}
		if (found && sctx.joinedSelectors && sctx.joinedSelectors->size()) {
			bool match = !start && count;
			if (!haveComparators && ctx.sortIndex) {
				assert(ctx.sortIndex->SortOrders().size() > static_cast<size_t>(val));
				realVal = ctx.sortIndex->SortOrders()[val];
			}

			// inner join process
			PayloadValue pv = ns_->items_[realVal];
			ConstPayload pl(ns_->payloadType_, pv);
			if (haveInnerJoin && sctx.joinedSelectors) {
				for (size_t i = 0; i < sctx.joinedSelectors->size(); i++) {
					auto &joinedSelector = sctx.joinedSelectors->at(i);
					bool res = false;
					joinedSelector.called++;

					if (joinedSelector.type == JoinType::InnerJoin) {
						if (found) {
							res = joinedSelector.func(realVal, pl, match);
							found &= res;
						}
					}
					if (joinedSelector.type == JoinType::OrInnerJoin) {
						if (!found || !joinedSelector.nodata) {
							res = joinedSelector.func(realVal, pl, match);
							found |= res;
						}
					}
					if (res) {
						joinedSelector.matched++;
					}
					// If not found, and next op is not OR, then ballout
					if (!found &&
						!(i + 1 < sctx.joinedSelectors->size() && sctx.joinedSelectors->at(i + 1).type == JoinType::OrInnerJoin)) {
						break;
					}
				}
			}
			// left join process
			if (match && found && sctx.joinedSelectors)
				for (auto &joinedSelector : *sctx.joinedSelectors)
					if (joinedSelector.type == JoinType::LeftJoin) joinedSelector.func(realVal, pl, match);
		}

		if (found) {
			sctx.matchedAtLeastOnce = true;
			// Check distinct condition:
			// Exclude last sets of id from each query result, so duplicated keys will
			// be removed
			for (auto &r : *ctx.qres)
				if (r.distinct) r.ExcludeLastSet();

			if (start)
				--start;
			else if (count) {
				if (!haveComparators && ctx.sortIndex) {
					assert(ctx.sortIndex->SortOrders().size() > static_cast<size_t>(val));
					realVal = ctx.sortIndex->SortOrders()[val];
				}
				--count;
				uint8_t proc = ft_ctx_ ? ft_ctx_->Proc(first.Pos()) : 0;

				if (aggregators.size()) {
					for (auto &aggregator : aggregators) aggregator.Aggregate(ns_->items_[realVal], realVal);
				} else if (sctx.preResult && sctx.preResult->mode == SelectCtx::PreResult::ModeBuild) {
					sctx.preResult->ids.Add(val, IdSet::Unordered);
				} else {
					result.Add({realVal, ns_->items_[realVal].GetVersion(), ns_->items_[realVal], proc, sctx.nsid});
				}
			}
			if (!count && !calcTotal) break;
			if (calcTotal) result.totalCount++;
		}
	}
	for (auto &aggregator : aggregators) {
		result.aggregationResults.push_back(aggregator.GetResult());
	}

	// Get total count for simple query with 1 condition and 1 idset
	if (ctx.calcTotal && !calcTotal) {
		if (sctx.query.entries.size()) {
			result.totalCount = (*ctx.qres)[0].GetMaxIterations();
		} else {
			result.totalCount = ns_->items_.size() - ns_->free_.size();
		}
	}
}

h_vector<Aggregator, 4> NsSelecter::getAggregators(const Query &q) {
	h_vector<Aggregator, 4> ret;

	for (auto &ag : q.aggregations_) {
		int idx = ns_->getIndexByName(ag.index_);
		ret.push_back(Aggregator(ns_->indexes_[idx]->KeyType(), ns_->indexes_[idx]->Opts().IsArray(), nullptr, ag.type_));
		ret.back().Bind(ns_->payloadType_, idx);
	}

	return ret;
}

void NsSelecter::substituteCompositeIndexes(QueryEntries &entries) {
	FieldsSet fields;
	for (auto cur = entries.begin(), first = entries.begin(); cur != entries.end(); cur++) {
		if ((cur->op != OpAnd) || (cur->condition != CondEq)) {
			// If query already rewritten, then copy current unmatched part
			first = cur + 1;
			fields.clear();
			continue;
		}
		fields.push_back(cur->idxNo);
		int found = getCompositeIndex(fields);
		if ((found >= 0) && !isFullText(ns_->indexes_[found]->Type())) {
			// composite idx found: replace conditions
			PayloadValue d(ns_->payloadType_.TotalSize());
			Payload pl(ns_->payloadType_, d);
			for (auto e = first; e != cur + 1; e++) {
				if (ns_->indexes_[found]->Fields().contains(e->idxNo)) {
					KeyRefs kr;
					kr.push_back(KeyRef(e->values[0]));
					pl.Set(e->idxNo, kr);
				} else
					*first++ = *e;
			}
			QueryEntry ce(OpAnd, CondEq, ns_->indexes_[found]->Name(), found);
			ce.values.push_back(KeyValue(d));
			*first++ = ce;
			cur = entries.erase(first, cur + 1);
			first = cur--;
			fields.clear();
		}
	}
}

void NsSelecter::updateCompositeIndexesValues(QueryEntries &qentries) {
	for (QueryEntry &qe : qentries) {
		if (qe.idxNo >= ns_->payloadType_.NumFields()) {
			for (KeyValue &kv : qe.values) {
				if (kv.Type() == KeyValueComposite) {
					kv.convertToComposite(ns_->payloadType_, ns_->indexes_[qe.idxNo]->Fields());
				}
			}
		}
	}
}

const string &NsSelecter::getOptimalSortOrder(const QueryEntries &entries) {
	Index *maxIdx = nullptr;
	static string no = "";
	for (auto c = entries.begin(); c != entries.end(); c++) {
		if (((c->idxNo != IndexValueType::SetByJsonPath) && (c->condition == CondGe || c->condition == CondGt || c->condition == CondLe ||
															 c->condition == CondLt || c->condition == CondRange)) &&
			!c->distinct && ns_->indexes_[c->idxNo]->IsOrdered()) {
			if (!maxIdx || ns_->indexes_[c->idxNo]->Size() > maxIdx->Size()) {
				maxIdx = ns_->indexes_[c->idxNo].get();
			}
		}
	}

	return maxIdx ? maxIdx->Name() : no;
}

int NsSelecter::getCompositeIndex(const FieldsSet &fields) {
	if (fields.getTagsPathsLength() == 0) {
		for (int i = ns_->indexes_.firstCompositePos(); i < ns_->indexes_.totalSize(); i++) {
			if (ns_->indexes_[i]->Fields().contains(fields)) return i;
		}
	}
	return -1;
}

bool NsSelecter::mergeQueryEntries(QueryEntry *lhs, QueryEntry *rhs) {
	if ((lhs->condition == CondEq || lhs->condition == CondSet) && (rhs->condition == CondEq || rhs->condition == CondSet)) {
		// intersect 2 queryenries on same index
		std::sort(lhs->values.begin(), lhs->values.end());
		lhs->values.erase(std::unique(lhs->values.begin(), lhs->values.end()), lhs->values.end());

		std::sort(rhs->values.begin(), rhs->values.end());
		rhs->values.erase(std::unique(rhs->values.begin(), rhs->values.end()), rhs->values.end());

		auto end =
			std::set_intersection(lhs->values.begin(), lhs->values.end(), rhs->values.begin(), rhs->values.end(), lhs->values.begin());
		lhs->values.resize(end - lhs->values.begin());
		lhs->condition = (lhs->values.size() == 1) ? CondEq : CondSet;
		lhs->distinct |= rhs->distinct;
		return true;
	} else if (rhs->condition == CondAny) {
		lhs->distinct |= rhs->distinct;
		return true;
	} else if (lhs->condition == CondAny) {
		rhs->distinct |= lhs->distinct;
		*lhs = std::move(*rhs);
		return true;
	}

	return false;
}

KeyValueType NsSelecter::getQueryEntryIndexType(const QueryEntry &qentry) const {
	KeyValueType keyType = KeyValueEmpty;
	if (!ns_->items_.empty()) {
		Payload pl(ns_->payloadType_, ns_->items_[0]);
		CJsonEncoder cjsonEncoder(ns_->tagsMatcher_, JsonPrintFilter());
		KeyRefs krefs = cjsonEncoder.ExtractFieldValue(&pl, qentry.index);
		if (krefs.size() > 0) keyType = krefs[0].Type();
	}
	return keyType;
}
}  // namespace reindexer
