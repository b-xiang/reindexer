#pragma once

#include <vector>
#include "libdivsufsort/divsufsort.h"

namespace reindexer {

using std::vector;

template <typename K, typename V>
class suffix_map {
	typedef size_t size_type;
	typedef unsigned char char_type;

	class value_type : public std::pair<const typename K::value_type *, V> {
	public:
		value_type(const std::pair<const typename K::value_type *, V> v) : std::pair<const typename K::value_type *, V>(v) {}
		const value_type *operator->() const { return this; }
	};

	class iterator {
		friend class suffix_map;

	public:
		iterator(size_type idx, const suffix_map *m) : idx_(idx), m_(m) {}
		iterator(const iterator &other) : idx_(other.idx_), m_(other.m_){};
		iterator &operator=(const iterator &other) {
			idx_ = other.idx;
			m_ = other.m_;
			return *this;
		}
		value_type operator->() {
			auto *p = &m_->text_[m_->sa_[idx_]];
			return value_type(std::make_pair(p, m_->mapped_[m_->sa_[idx_]]));
		}

		const value_type operator->() const {
			auto *p = &m_->text_[m_->sa_[idx_]];
			return value_type(std::make_pair(p, m_->mapped_[m_->sa_[idx_]]));
		}

		iterator &operator++() {
			++idx_;
			return *this;
		}
		iterator &operator--() {
			--idx_;
			return *this;
		}
		iterator operator++(int) {
			iterator ret = *this;
			idx_++;
			return ret;
		}
		iterator operator--(int) {
			iterator ret = *this;
			idx_--;
			return ret;
		}
		int lcp() { return m_->lcp_[idx_]; }
		bool operator!=(const iterator &rhs) const { return idx_ != rhs.idx_; }
		bool operator==(const iterator &rhs) const { return idx_ == rhs.idx_; }

	protected:
		size_type idx_;
		const suffix_map *m_;
	};

public:
	iterator begin() const { return iterator(0, this); }
	iterator end() const { return iterator(sa_.size(), this); }

	std::pair<iterator, iterator> match_range(const K &str) const {
		iterator start = lower_bound(str);
		if (start == end()) return {end(), end()};
		int idx_ = start.idx_ + 1;
		while (idx_ < int(sa_.size()) && lcp_[idx_ - 1] >= int(str.length())) idx_++;
		return {start, iterator(idx_, this)};
	}

	iterator lower_bound(const K &str) const {
		if (!built_) throw std::logic_error("Should call suffix_map::build before search");

		size_type lo = 0, hi = sa_.size(), mid;
		int lcp_lo = 0, lcp_hi = 0;
		auto P = reinterpret_cast<const char_type *>(str.c_str());
		auto T = reinterpret_cast<const char_type *>(text_.c_str());
		while (lo <= hi) {
			mid = (lo + hi) / 2;
			int i = std::min(lcp_hi, lcp_lo);
			bool plt = true;
			if (mid >= sa_.size()) return end();
			while (i < int(str.length()) && sa_[mid] + i < int(text_.length())) {
				if (P[i] < T[sa_[mid] + i]) {
					break;
				} else if (P[i] > T[sa_[mid] + i]) {
					plt = false;
					break;
				}
				i++;
			}
			if (plt) {
				if (mid == lo + 1) {
					if (strncmp(str.c_str(), &text_[sa_[mid]], std::min(str.length(), strlen(&text_[sa_[mid]])))) return end();
					return iterator(mid, this);
				}
				lcp_hi = i;
				hi = mid;
			} else {
				if (mid == hi - 1) {
					if (hi >= sa_.size() || strncmp(str.c_str(), &text_[sa_[hi]], std::min(str.length(), strlen(&text_[sa_[hi]]))))
						return end();
					return iterator(hi, this);
				}
				lcp_lo = i;
				lo = mid;
			}
		}
		return end();
	}

	int insert(const K &word, const V &val) {
		int wpos = text_.length();
		text_ += word + '\0';
		mapped_.insert(mapped_.end(), word.length() + 1, val);
		words_.push_back(wpos);
		words_len_.push_back(word.length());
		built_ = false;
		return wpos;
	};

	const typename K::value_type *word_at(int idx) const { return &text_[words_[idx]]; }
	int word_len_at(int idx) const { return words_len_[idx]; }

	void build() {
		if (built_) return;
		text_.shrink_to_fit();
		sa_.resize(text_.length());
		::divsufsort(reinterpret_cast<const char_type *>(text_.c_str()), &sa_[0], text_.length());
		build_lcp();
		built_ = true;
	}

	void reserve(size_type sz_text, size_type sz_words) {
		text_.reserve(sz_text + 1);
		mapped_.reserve(sz_text + 1);
		words_.reserve(sz_words);
		words_len_.reserve(sz_words);
	}
	void clear() {
		sa_.clear();
		lcp_.clear();
		mapped_.clear();
		words_.clear();
		words_len_.clear();
		text_.clear();
		built_ = false;
	}
	size_type size() { return sa_.size(); }
	const K &text() const { return text_; }

protected:
	void build_lcp() {
		vector<int> rank_;
		rank_.resize(sa_.size());
		lcp_.resize(sa_.size());
		int k = 0, n = size();

		for (int i = 0; i < n; i++) rank_[sa_[i]] = i;
		for (int i = 0; i < n; i++, k ? k-- : 0) {
			if (rank_[i] == n - 1) {
				k = 0;
				continue;
			}
			int j = sa_[rank_[i] + 1];
			while (i + k < n && j + k < n && text_[i + k] == text_[j + k]) k++;
			lcp_[rank_[i]] = k;
		}
	}

	std::vector<int> sa_, words_;
	std::vector<int16_t> lcp_, words_len_;
	vector<V> mapped_;
	K text_;
	bool built_ = false;
};

}  // namespace reindexer
