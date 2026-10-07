//
//	License type: BSD 3-Clause License
//	License copy: https://github.com/Telecominfraproject/wlan-cloud-ucentralgw/blob/master/LICENSE
//
//	Created by Stephane Bourque on 2021-03-04.
//	Arilia Wireless Inc.
//

#pragma once

#include <array>
#include <exception>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "Poco/Data/RecordSet.h"
#include "Poco/Data/SQLite/Connector.h"
#include "Poco/Data/SessionPool.h"
#include "Poco/Data/Statement.h"
#include "Poco/Logger.h"
#include "Poco/StringTokenizer.h"
#include "Poco/Tuple.h"
#include "DbTransaction.h"
#include "StorageClass.h"

#include "fmt/format.h"

namespace ORM {

	enum FieldType { FT_INT, FT_BIGINT, FT_TEXT, FT_VARCHAR, FT_BLOB, FT_BOOLEAN, FT_REAL };

	enum Indextype { ASC, DESC };

	struct Field {
		std::string Name;
		FieldType Type;
		int Size = 0;
		bool Index = false;

		Field(std::string N, FieldType T, int S = 0, bool Index = false)
			: Name(std::move(N)), Type(T), Size(S), Index(Index) {}

		explicit Field(std::string N) : Name(std::move(N)) { Type = FT_TEXT; }

		Field(std::string N, int S) : Name(std::move(N)), Size(S) {
			if (Size > 0 && Size < 255)
				Type = FT_VARCHAR;
			else
				Type = FT_TEXT;
		}

		Field(std::string N, int S, bool I) : Name(std::move(N)), Size(S), Index(I) {
			if (Size > 0 && Size < 255)
				Type = FT_VARCHAR;
			else
				Type = FT_TEXT;
		}
	};
	typedef std::vector<Field> FieldVec;

	struct IndexEntry {
		std::string FieldName;
		Indextype Type;
	};
	typedef std::vector<IndexEntry> IndexEntryVec;

	struct Index {
		std::string Name;
		IndexEntryVec Entries;
		// When true, generates PostgreSQL CREATE UNIQUE INDEX (unsupported on SQLite/MySQL). Defaults to false.
		bool Unique = false;
	};
	typedef std::vector<Index> IndexVec;

	inline std::string FieldTypeToChar(OpenWifi::DBType Type, FieldType T, int Size = 0) {
		switch (T) {
		case FT_INT:
			return "INT";
		case FT_BIGINT:
			return "BIGINT";
		case FT_TEXT:
			return "TEXT";
		case FT_BOOLEAN:
			return "BOOLEAN";
		case FT_VARCHAR:
			if (Size)
				return std::string("VARCHAR(") + std::to_string(Size) + std::string(")");
			else
				return "TEXT";
		case FT_BLOB:
			if (Type == OpenWifi::DBType::mysql)
				return "LONGBLOB";
			else if (Type == OpenWifi::DBType::pgsql)
				return "BYTEA";
			else
				return "BLOB";
		case FT_REAL:
			return "REAL";
		default:
			assert(false);
		}
		assert(false);
		return "";
	}

	inline std::string Escape(const std::string &S) {
		std::string R;

		for (const auto &i : S) {
			if (i == '\'')
				R += "''";
			else
				R += i;
		}
		return R;
	}

	inline std::string WHERE_AND_(std::string Result) { return Result; }

	template <typename T, typename... Args>
	std::string WHERE_AND_(std::string Result, const char *fieldName, const T &Value,
						   Args... args) {
		if constexpr (std::is_same_v<T, std::string>) {
			if (!Value.empty()) {
				if (!Result.empty())
					Result += " and ";
				Result += fieldName;
				Result += '=';
				Result += "'";
				Result += Escape(Value);
				Result += "'";
			}
			return WHERE_AND_(Result, args...);
		} else if constexpr (std::is_same_v<T, const char *>) {
			if (*Value != 0) {
				if (!Result.empty())
					Result += " and ";
				Result += fieldName;
				Result += '=';
				Result += "'";
				Result += Escape(Value);
				Result += "'";
			}
			return WHERE_AND_(Result, args...);
		} else if constexpr (std::is_same_v<T, bool>) {
			if (!Result.empty())
				Result += " and ";
			Result += fieldName;
			Result += '=';
			Result += Value ? "true" : "false";
			return WHERE_AND_(Result, args...);
		} else if constexpr (std::is_arithmetic_v<T>) {
			if (!Result.empty())
				Result += " and ";
			Result += fieldName;
			Result += '=';
			Result += std::to_string(Value);
			return WHERE_AND_(Result, args...);
		} else {
			assert(false);
		}
		return WHERE_AND_(Result, args...);
	}

	template <typename... Args> std::string WHERE_AND(Args... args) {
		std::string Result;
		return WHERE_AND_(Result, args...);
	}

	enum SqlComparison { EQ = 0, NEQ, LT, LTE, GT, GTE };
	enum SqlBinaryOp { AND = 0, OR };

	static const std::vector<std::string> BOPS{" and ", " or "};
	static const std::vector<std::string> SQLCOMPS{"=", "!=", "<", "<=", ">", ">="};

	inline std::string to_string(uint64_t V) { return std::to_string(V); }

	inline std::string to_string(int V) { return std::to_string(V); }

	inline std::string to_string(bool V) { return std::to_string(V); }

	inline std::string to_string(const std::string &S) { return S; }

	inline std::string to_string(const Poco::Data::BLOB &blob) {
		std::string result;
		result.assign(blob.begin(), blob.end());
		return result;
	}

	inline std::string to_string(const char *S) { return S; }

	template <typename RecordType> class DBCache {
	  public:
		DBCache(unsigned Size, unsigned Timeout) : Size_(Size), Timeout_(Timeout) {}
		virtual void Create(const RecordType &R) = 0;
		virtual bool GetFromCache(const std::string &FieldName, const std::string &Value,
								  RecordType &R) = 0;
		virtual void UpdateCache(const RecordType &R) = 0;
		virtual void Delete(const std::string &FieldName, const std::string &Value) = 0;

		// Last-resort consistency recovery when targeted cache invalidation fails.
		// Post-commit cache recovery is best-effort and does not alter a successful DB commit.
		virtual void InvalidateAll() = 0;

	  private:
		size_t Size_ = 0;
		uint64_t Timeout_ = 0;
	};

	template <typename RecordTuple, typename RecordType> class DB {
	  public:
		typedef const char *field_name_t;

		DB(OpenWifi::DBType dbtype, const char *TableName, const FieldVec &Fields,
		   const IndexVec &Indexes, Poco::Data::SessionPool &Pool, Poco::Logger &L,
		   const char *Prefix, DBCache<RecordType> *Cache = nullptr)
			: TableName_(TableName), Type_(dbtype), Pool_(Pool), Logger_(L), Prefix_(Prefix),
			  Cache_(Cache) {
			assert(RecordTuple::length == Fields.size());

			bool first = true;
			int Place = 0;

			for (const auto &i : Fields) {
				std::string FieldName = Poco::toLower(i.Name);
				FieldNames_[FieldName] = Place;
				if (!first) {
					CreateFields_ += ", ";
					SelectFields_ += ", ";
					UpdateFields_ += ", ";
					SelectList_ += ", ";
				} else {
					SelectList_ += "(";
				}

				CreateFields_ += FieldName + " " + FieldTypeToChar(Type_, i.Type, i.Size) +
								 (i.Index ? " unique primary key" : "");
				SelectFields_ += FieldName;
				UpdateFields_ += FieldName + "=?";
				SelectList_ += "?";
				first = false;
				Place++;
			}
			SelectList_ += ")";

			if (!Indexes.empty()) {
				if (Type_ == OpenWifi::DBType::pgsql) {
					for (const auto &j : Indexes) {
						std::string IndexLine;

						// Use CREATE UNIQUE INDEX for indexes declared with Unique=true;
						// fall back to the plain non-unique form for all existing declarations.
						IndexLine = std::string(j.Unique ? "CREATE UNIQUE INDEX IF NOT EXISTS "
						                                 : "CREATE INDEX IF NOT EXISTS ") +
						            j.Name + " ON " + TableName_ + " (";
						bool first_entry = true;
						for (const auto &k : j.Entries) {
							auto IndexFieldName = Poco::toLower(k.FieldName);
							assert(ValidFieldName(IndexFieldName));
							if (!first_entry) {
								IndexLine += " , ";
							}
							first_entry = false;
							IndexLine += IndexFieldName + (k.Type == Indextype::ASC ? " ASC" : " DESC");
						}
						IndexLine += " )";
						IndexCreation_.template emplace_back(IndexLine);
						// Mirror entry so Create() can validate columns of unique indexes.
						PgsqlIndexDefs_.push_back(j);
					}
				} else if (Type_ == OpenWifi::DBType::sqlite) {
					for (const auto &j : Indexes) {
						if (j.Unique) {
							HasUnsupportedUniqueIndex_ = true;
							continue;
						}
						std::string IndexLine;

						IndexLine = std::string("CREATE INDEX IF NOT EXISTS ") + j.Name +
									std::string(" ON ") + TableName_ + " (";
						bool first_entry = true;
						for (const auto &k : j.Entries) {
							auto IndexFieldName = Poco::toLower(k.FieldName);
							assert(ValidFieldName(IndexFieldName));
							if (!first_entry) {
								IndexLine += " , ";
							}
							first_entry = false;
							IndexLine += IndexFieldName + std::string(" ") +
										 std::string(k.Type == Indextype::ASC ? "ASC" : "DESC");
						}
						IndexLine += " )";
						IndexCreation_.template emplace_back(IndexLine);
					}
				} else if (Type_ == OpenWifi::DBType::mysql) {
					bool firstIndex = true;
					std::string IndexLine;
					for (const auto &j : Indexes) {
						if (j.Unique) {
							HasUnsupportedUniqueIndex_ = true;
							continue;
						}
						if (!firstIndex)
							IndexLine += ", ";
						firstIndex = false;
						IndexLine += " INDEX " + j.Name + " ( ";
						bool first_entry = true;
						for (const auto &k : j.Entries) {
							auto IndexFieldName = Poco::toLower(k.FieldName);
							assert(FieldNames_.find(IndexFieldName) != FieldNames_.end());
							if (!first_entry) {
								IndexLine += " ,";
							}
							first_entry = false;
							IndexLine += IndexFieldName +
										 std::string(k.Type == Indextype::ASC ? " ASC" : " DESC");
						}
						IndexLine += " ) ";
					}
					IndexCreation_.template emplace_back(IndexLine);
				}
			}
		}

		[[nodiscard]] const std::string &CreateFields() const { return CreateFields_; };
		[[nodiscard]] const std::string &SelectFields() const { return SelectFields_; };
		[[nodiscard]] const std::string &SelectList() const { return SelectList_; };
		[[nodiscard]] const std::string &UpdateFields() const { return UpdateFields_; };

		inline std::string OP(field_name_t F, SqlComparison O, bool V) {
			assert(ValidFieldName(F));
			return std::string{"("} + F + SQLCOMPS[O] + (V ? "true" : "false") + ")";
		}

		inline std::string OP(field_name_t F, SqlComparison O, int V) {
			assert(ValidFieldName(F));
			return std::string{"("} + F + SQLCOMPS[O] + std::to_string(V) + ")";
		}

		inline std::string OP(field_name_t F, SqlComparison O, uint64_t V) {
			assert(ValidFieldName(F));
			return std::string{"("} + F + SQLCOMPS[O] + std::to_string(V) + ")";
		}

		std::string OP(field_name_t F, SqlComparison O, const std::string &V) {
			assert(ValidFieldName(F));
			return std::string{"("} + F + SQLCOMPS[O] + "'" + Escape(V) + "')";
		}

		std::string OP(field_name_t F, SqlComparison O, const char *V) {
			assert(ValidFieldName(F));
			return std::string{"("} + F + SQLCOMPS[O] + "'" + Escape(V) + "')";
		}

		static std::string OP(const std::string &P1, SqlBinaryOp BOP, const std::string &P2) {
			return std::string("(") + P1 + BOPS[BOP] + P2 + ")";
		}

		std::string OP([[maybe_unused]] bool Paran, const std::string &P1, SqlBinaryOp BOP,
					   const std::string &P2) {
			return P1 + BOPS[BOP] + P2 + ")";
		}

		template <typename... Others>
		std::string OP(bool ParanOpen, const std::string &P1, SqlBinaryOp BOP,
					   const std::string &P2, Others... More) {
			return P1 + BOPS[BOP] + OP(ParanOpen, P2, More...) + ")";
		}

		template <typename... Others>
		std::string OP(const std::string &P1, SqlBinaryOp BOP, const std::string &P2,
					   Others... More) {
			return std::string{"("} + P1 + BOPS[BOP] + OP(true, P2, More...);
		}

		bool Upgrade() {
			uint32_t To;
			return Upgrade(0, To);
		}

		inline bool Create() {
			if (HasUnsupportedUniqueIndex_) {
				Logger_.error("Unique secondary indexes are only supported on PostgreSQL. "
				              "Cannot create table '" + TableName_ + "' because one or more declared "
				              "indexes require unique constraint support on an unsupported database backend.");
				return false;
			}
			switch (Type_) {
			case OpenWifi::DBType::mysql: {
				try {
					Poco::Data::Session Session = Pool_.get();
					std::string Statement = IndexCreation_.empty()
												? "create table if not exists " + TableName_ +
													  " ( " + CreateFields_ + " )"
												: "create table if not exists " + TableName_ +
													  " ( " + CreateFields_ + " ), " +
													  IndexCreation_[0] + " )";
					Session << Statement, Poco::Data::Keywords::now;
				} catch (const Poco::Exception &E) {
					Logger_.error("Failure to create MySQL DB resources.");
					Logger_.log(E);
				}
			} break;

			case OpenWifi::DBType::sqlite: {
				try {
					Poco::Data::Session Session = Pool_.get();
					std::string Statement =
						"create table if not exists " + TableName_ + " ( " + CreateFields_ + " )";
					Session << Statement, Poco::Data::Keywords::now;
					for (const auto &i : IndexCreation_) {
						Session << i, Poco::Data::Keywords::now;
					}
				} catch (const Poco::Exception &E) {
					Logger_.error("Failure to create SQLITE DB resources.");
					Logger_.log(E);
				}
			} break;

			case OpenWifi::DBType::pgsql: {
				try {
					Poco::Data::Session Session = Pool_.get();
					std::string Statement = "create table if not exists " + TableName_ + " ( " + CreateFields_ + " )";
					Session << Statement, Poco::Data::Keywords::now;
					for (std::size_t Idx = 0; Idx < IndexCreation_.size(); ++Idx) {
						const auto &i = IndexCreation_[Idx];
						if (Idx < PgsqlIndexDefs_.size() && PgsqlIndexDefs_[Idx].Unique) {
							const auto &Decl = PgsqlIndexDefs_[Idx];
							try {
								Session << i, Poco::Data::Keywords::now;
							} catch (const Poco::Exception &E) {
								Logger_.error("Failure to create PostgreSQL unique index on table '" + TableName_ + "'.");
								Logger_.log(E);
								return false;
							} catch (const std::exception &E) {
								Logger_.error("Failure to create PostgreSQL unique index on table '" + TableName_ + "': " + std::string(E.what()));
								return false;
							}
							try {
								bool IsUnique = false, IsValid = false, IsNotPartial = false, IsNotExpression = false;
								int AttCount = 0;
								std::string ActualCols, ExpectedCols;
								for (const auto &entry : Decl.Entries) {
									if (!ExpectedCols.empty()) ExpectedCols += ',';
									ExpectedCols += Poco::toLower(entry.FieldName);
								}
								std::string VerifyQ = ConvertParams(
									"SELECT i.indisunique, i.indisvalid, (i.indpred IS NULL), (i.indexprs IS NULL), "
									"CAST(i.indnatts AS int), string_agg(lower(a.attname), ',' ORDER BY pos.pos) "
									"FROM pg_index i "
									"JOIN pg_class idx_cls ON idx_cls.oid = i.indexrelid "
									"JOIN pg_class tbl_cls ON tbl_cls.oid = i.indrelid "
									"JOIN LATERAL unnest(i.indkey) WITH ORDINALITY AS pos(attnum, pos) ON true "
									"JOIN pg_attribute a ON a.attrelid = i.indrelid AND a.attnum = pos.attnum "
									"WHERE idx_cls.relname = ? AND tbl_cls.relname = ? "
									"GROUP BY i.indisunique, i.indisvalid, (i.indpred IS NULL), (i.indexprs IS NULL), i.indnatts");
								std::string UseIndexName = Decl.Name;
								std::string UseTableName = TableName_;
								Session << VerifyQ,
								    Poco::Data::Keywords::into(IsUnique),
								    Poco::Data::Keywords::into(IsValid),
								    Poco::Data::Keywords::into(IsNotPartial),
								    Poco::Data::Keywords::into(IsNotExpression),
								    Poco::Data::Keywords::into(AttCount),
								    Poco::Data::Keywords::into(ActualCols),
								    Poco::Data::Keywords::use(UseIndexName),
								    Poco::Data::Keywords::use(UseTableName),
								    Poco::Data::Keywords::now;
								if (!IsUnique || !IsValid || !IsNotPartial || !IsNotExpression ||
								    AttCount != static_cast<int>(Decl.Entries.size()) || ActualCols != ExpectedCols) {
									Logger_.error("PostgreSQL unique index '" + Decl.Name + "' validation failed on table '" + TableName_ + "'.");
									return false;
								}
							} catch (const Poco::Exception &E) {
								Logger_.error("Failed to verify PostgreSQL unique index '" + Decl.Name + "' on table '" + TableName_ + "'.");
								Logger_.log(E);
								return false;
							}
						} else {
							Session << i, Poco::Data::Keywords::now;
						}
					}
				} catch (const Poco::Exception &E) {
					Logger_.error("Failure to create POSTGRESQL DB resources.");
					Logger_.log(E);
				}
			} break;
			}
			return Upgrade();
		}

		[[nodiscard]] std::string ConvertParams(const std::string &S) const {
			if (Type_ != OpenWifi::DBType::pgsql)
				return S;

			std::string R;
			R.reserve(S.size() * 2 + 1);
			auto Idx = 1;
			for (auto const &i : S) {
				if (i == '?') {
					R += '$';
					R.append(std::to_string(Idx++));
				} else {
					R += i;
				}
			}

			return R;
		}

		void Convert(const RecordTuple &in, RecordType &out);
		void Convert(const RecordType &in, RecordTuple &out);

		inline const std::string &Prefix() { return Prefix_; };

		bool CreateRecord(const RecordType &R) {
			try {
				Poco::Data::Session Session = Pool_.get();
				Poco::Data::Statement Insert(Session);

				RecordTuple RT;
				Convert(R, RT);
				std::string St = "insert into  " + TableName_ + " ( " + SelectFields_ +
								 " ) values " + SelectList_;
				Insert << ConvertParams(St), Poco::Data::Keywords::use(RT);
				Insert.execute();

				if (Cache_)
					Cache_->Create(R);
				return true;

			} catch (const Poco::Exception &E) {
				Logger_.log(E);
			}
			return false;
		}

		template <typename T>
		bool GetRecord(field_name_t FieldName, const T &Value, RecordType &R) {
			try {
				assert(ValidFieldName(FieldName));

				if (Cache_) {
					if (Cache_->GetFromCache(FieldName, Value, R))
						return true;
				}

				Poco::Data::Session Session = Pool_.get();
				Poco::Data::Statement Select(Session);
				RecordTuple RT;

				std::string St = "select " + SelectFields_ + " from " + TableName_ + " where " +
								 FieldName + "=?" + " limit 1";

				auto tValue{Value};

				Select << ConvertParams(St), Poco::Data::Keywords::into(RT),
					Poco::Data::Keywords::use(tValue);
				Select.execute();

				if (Select.execute() == 1) {
					Convert(RT, R);
					if (Cache_)
						Cache_->UpdateCache(R);
					return true;
				}
			} catch (const Poco::Exception &E) {
				Logger_.log(E);
			}
			return false;
		}

		bool GetRecord(RecordType &T, const std::string &WhereClause) {
			try {
				Poco::Data::Session Session = Pool_.get();
				Poco::Data::Statement Select(Session);
				RecordTuple RT;

				std::string St = "select " + SelectFields_ + " from " + TableName_ + " where " +
								 WhereClause + " limit 1";

				Select << ConvertParams(St), Poco::Data::Keywords::into(RT);
				Select.execute();

				if (Select.execute() == 1) {
					Convert(RT, T);
					if (Cache_)
						Cache_->UpdateCache(T);
					return true;
				}
			} catch (const Poco::Exception &E) {
				Logger_.log(E);
			}
			return false;
		}

		typedef std::vector<std::string> StringVec;

		template <typename T, typename T0, typename T1>
		bool GR(field_name_t FieldName, T &R, T0 &V0, T1 &V1) {
			try {

				assert(ValidFieldName(FieldName));

				Poco::Data::Session Session = Pool_.get();
				Poco::Data::Statement Select(Session);
				RecordTuple RT;

				std::string St = "select " + SelectFields_ + " from " + TableName_ + " where " +
								 FieldName[0] + "=? and " + FieldName[1] + "=?";
				Select << ConvertParams(St), Poco::Data::Keywords::into(RT),
					Poco::Data::Keywords::use(V0), Poco::Data::Keywords::use(V1);

				if (Select.execute() == 1) {
					Convert(RT, R);
					return true;
				}
				return true;
			} catch (const Poco::Exception &E) {
				Logger_.log(E);
			}
			return false;
		}

		template <typename T> bool Join(const std::string &statement, std::vector<T> &records) {
			try {
				Poco::Data::Session Session = Pool_.get();
				Poco::Data::Statement Select(Session);

				Select << statement, Poco::Data::Keywords::into(records);
				Select.execute();
				return true;
			} catch (const Poco::Exception &E) {
				Logger_.log(E);
			}
			return false;
		}

		typedef std::vector<RecordTuple> RecordList;
		typedef std::vector<RecordType> RecordVec;
		typedef RecordType RecordName;

		bool GetRecords(uint64_t Offset, uint64_t HowMany, RecordVec &Records,
						const std::string &Where = "", const std::string &OrderBy = "") {
			try {
				Poco::Data::Session Session = Pool_.get();
				Poco::Data::Statement Select(Session);
				RecordList RL;
				std::string St = "select " + SelectFields_ + " from " + TableName_ +
								 (Where.empty() ? "" : " where " + Where) + OrderBy +
								 ComputeRange(Offset, HowMany);

				Select << St, Poco::Data::Keywords::into(RL);
				Select.execute();

				if (Select.rowsExtracted() > 0) {
					for (auto &i : RL) {
						RecordType R;
						Convert(i, R);
						Records.template emplace_back(R);
					}
					return true;
				}
				return false;
			} catch (const Poco::Exception &E) {
				Logger_.log(E);
			}
			return false;
		}

		template <typename T>
		bool UpdateRecord(field_name_t FieldName, const T &Value, const RecordType &R) {
			try {
				assert(ValidFieldName(FieldName));
				Poco::Data::Session Session = Pool_.get();
                Session.begin();
				Poco::Data::Statement Update(Session);

				RecordTuple RT;

				Convert(R, RT);

				auto tValue(Value);

				std::string St =
					"update " + TableName_ + " set " + UpdateFields_ + " where " + FieldName + "=?";
				Update << ConvertParams(St), Poco::Data::Keywords::use(RT),
					Poco::Data::Keywords::use(tValue);
				Update.execute();
				if (Cache_)
					Cache_->UpdateCache(R);
                Session.commit();
				return true;
			} catch (const Poco::Exception &E) {
				Logger_.log(E);
			}
			return false;
		}

		bool RunStatement(const std::string &St) {
			try {
				Poco::Data::Session Session = Pool_.get();
				Poco::Data::Statement Command(Session);

				Command << St;
				Command.execute();

				return true;
			} catch (const Poco::Exception &E) {
				Logger_.log(E);
			}
			return false;
		}

		template <typename T>
		bool ReplaceRecord(field_name_t FieldName, const T &Value, RecordType &R) {
			try {
				if (Exists(FieldName, Value)) {
					return UpdateRecord(FieldName, Value, R);
				}
				return CreateRecord(R);
			} catch (const Poco::Exception &E) {
				Logger_.log(E);
			}
			return false;
		}

		template <typename T>
		bool GetNameAndDescription(field_name_t FieldName, const T &Value, std::string &Name,
								   std::string &Description) {
			try {
				assert(ValidFieldName(FieldName));
				Poco::Data::Session Session = Pool_.get();
				Poco::Data::Statement Select(Session);
				RecordTuple RT;

				std::string St = "select " + SelectFields_ + " from " + TableName_ + " where " +
								 FieldName + "=?";
				RecordType R;
				auto tValue{Value};
				Select << ConvertParams(St), Poco::Data::Keywords::into(RT),
					Poco::Data::Keywords::use(tValue);

				if (Select.execute() == 1) {
					Convert(RT, R);
					Name = R.info.name;
					Description = R.info.description;
					return true;
				}
				return false;
			} catch (const Poco::Exception &E) {
				Logger_.log(E);
			}
			return false;
		}

		template <typename T> bool DeleteRecord(field_name_t FieldName, const T &Value) {
			try {
				assert(ValidFieldName(FieldName));

				Poco::Data::Session Session = Pool_.get();
                Session.begin();
				Poco::Data::Statement Delete(Session);

				std::string St = "delete from " + TableName_ + " where " + FieldName + "=?";
				auto tValue{Value};

				Delete << ConvertParams(St), Poco::Data::Keywords::use(tValue);
				Delete.execute();
				if (Cache_)
					Cache_->Delete(FieldName, Value);
                Session.commit();
				return true;
			} catch (const Poco::Exception &E) {
				Logger_.log(E);
			}
			return false;
		}

		bool DeleteRecords(const std::string &WhereClause) {
			try {
				assert(!WhereClause.empty());
				Poco::Data::Session Session = Pool_.get();
                Session.begin();
				Poco::Data::Statement Delete(Session);

				std::string St = "delete from " + TableName_ + " where " + WhereClause;
				Delete << St;
				Delete.execute();
                Session.commit();
				return true;
			} catch (const Poco::Exception &E) {
				Logger_.log(E);
			}
			return false;
		}

		bool Exists(field_name_t FieldName, const std::string &Value) {
			try {
				assert(ValidFieldName(FieldName));

				RecordType R;
				if (GetRecord(FieldName, Value, R))
					return true;
				return false;
			} catch (const Poco::Exception &E) {
				Logger_.log(E);
			}
			return false;
		}

		bool Iterate(std::function<bool(const RecordType &R)> F,
					 const std::string &WhereClause = "") {
			try {

				uint64_t Offset = 0;
				uint64_t Batch = 50;
				bool Done = false;
				while (!Done) {
					std::vector<RecordType> Records;
					if (GetRecords(Offset, Batch, Records, WhereClause)) {
						for (const auto &i : Records) {
							if (!F(i))
								return true;
						}
						if (Records.size() < Batch)
							return true;
						Offset += Batch;
					} else {
						Done = true;
					}
				}
				return true;
			} catch (const Poco::Exception &E) {
				Logger_.log(E);
			}
			return false;
		}

		bool PrepareOrderBy(const std::string &OrderByList, std::string &OrderByString) {
			auto items = Poco::StringTokenizer(OrderByList, ",");
			std::string ItemList;

			for (const auto &i : items) {
				auto T = Poco::StringTokenizer(i, ":");
				if (T.count() != 2) {
					return false;
				}
				if (T[1] != "a" && T[1] != "d") {
					return false;
				}
				if (!ItemList.empty())
					ItemList += " , ";
				auto hint = FieldNames_.find(Poco::toLower(T[0]));
				if (hint == FieldNames_.end()) {
					return false;
				}
				ItemList += T[0] + (T[1] == "a" ? " ASC" : " DESC");
			}

			if (!ItemList.empty()) {
				OrderByString = " ORDER BY " + ItemList;
			}
			return true;
		}

		uint64_t Count(const std::string &Where = "") {
			try {
				uint64_t Cnt = 0;

				Poco::Data::Session Session = Pool_.get();
				Poco::Data::Statement Select(Session);

				std::string st{"SELECT COUNT(*) FROM " + TableName_ + " " +
							   (Where.empty() ? "" : (" where " + Where))};

				Select << st, Poco::Data::Keywords::into(Cnt);
				Select.execute();

				return Cnt;

			} catch (const Poco::Exception &E) {
				Logger_.log(E);
			}
			return 0;
		}

		template <typename X>
		bool ManipulateVectorMember(X T, field_name_t FieldName, const std::string &ParentUUID,
									const std::string &ChildUUID, bool Add) {
			try {
				assert(ValidFieldName(FieldName));

				RecordType R;
				if (GetRecord(FieldName, ParentUUID, R)) {
					auto it = std::find((R.*T).begin(), (R.*T).end(), ChildUUID);
					if (Add) {
						if (it != (R.*T).end() && *it == ChildUUID)
							return false;
						(R.*T).push_back(ChildUUID);
						std::sort((R.*T).begin(), (R.*T).end());
					} else {
						if (it != (R.*T).end() && *it == ChildUUID)
							(R.*T).erase(it);
						else
							return false;
					}
					UpdateRecord(FieldName, ParentUUID, R);
					return true;
				}
			} catch (const Poco::Exception &E) {
				Logger_.log(E);
			}
			return false;
		}

		bool RunScript(const std::vector<std::string> &Statements, bool IgnoreExceptions = true) {
			try {
				Poco::Data::Session Session = Pool_.get();
				Poco::Data::Statement Command(Session);

				for (const auto &i : Statements) {
					try {
						Command << i, Poco::Data::Keywords::now;
					} catch (const Poco::Exception &E) {
						// Logger_.log(E);
						// Logger_.error(Poco::format("The following statement '%s' generated an
						// exception during a table upgrade. This may or may not be a problem.",
						// i));
						if (!IgnoreExceptions) {
							return false;
						}
					}
					Command.reset(Session);
				}
				return true;
			} catch (const Poco::Exception &E) {
				Logger_.log(E);
			}
			return false;
		}

		virtual uint32_t Version() { return 0; }

		virtual bool Upgrade(uint32_t from, uint32_t &to) {
			to = from;
			return true;
		}

		inline bool AddChild(field_name_t FieldName, const std::string &ParentUUID,
							 const std::string &ChildUUID) {
			return ManipulateVectorMember(&RecordType::children, FieldName, ParentUUID, ChildUUID,
										  true);
		}

		inline bool DeleteChild(field_name_t FieldName, const std::string &ParentUUID,
								const std::string &ChildUUID) {
			return ManipulateVectorMember(&RecordType::children, FieldName, ParentUUID, ChildUUID,
										  false);
		}

		inline bool AddLocation(field_name_t FieldName, const std::string &ParentUUID,
								const std::string &ChildUUID) {
			return ManipulateVectorMember(&RecordType::locations, FieldName, ParentUUID, ChildUUID,
										  true);
		}

		inline bool DeleteLocation(field_name_t FieldName, const std::string &ParentUUID,
								   const std::string &ChildUUID) {
			return ManipulateVectorMember(&RecordType::locations, FieldName, ParentUUID, ChildUUID,
										  false);
		}

		inline bool AddContact(field_name_t FieldName, const std::string &ParentUUID,
							   const std::string &ChildUUID) {
			return ManipulateVectorMember(&RecordType::contacts, FieldName, ParentUUID, ChildUUID,
										  true);
		}

		inline bool DeleteContact(field_name_t FieldName, const std::string &ParentUUID,
								  const std::string &ChildUUID) {
			return ManipulateVectorMember(&RecordType::contacts, FieldName, ParentUUID, ChildUUID,
										  false);
		}

		inline bool AddVenue(field_name_t FieldName, const std::string &ParentUUID,
							 const std::string &ChildUUID) {
			return ManipulateVectorMember(&RecordType::venues, FieldName, ParentUUID, ChildUUID,
										  true);
		}

		inline bool DeleteVenue(field_name_t FieldName, const std::string &ParentUUID,
								const std::string &ChildUUID) {
			return ManipulateVectorMember(&RecordType::venues, FieldName, ParentUUID, ChildUUID,
										  false);
		}

		inline bool AddDevice(field_name_t FieldName, const std::string &ParentUUID,
							  const std::string &ChildUUID) {
			return ManipulateVectorMember(&RecordType::devices, FieldName, ParentUUID, ChildUUID,
										  true);
		}

		inline bool DeleteDevice(field_name_t FieldName, const std::string &ParentUUID,
								 const std::string &ChildUUID) {
			return ManipulateVectorMember(&RecordType::devices, FieldName, ParentUUID, ChildUUID,
										  false);
		}

		inline bool AddEntity(field_name_t FieldName, const std::string &ParentUUID,
							  const std::string &ChildUUID) {
			return ManipulateVectorMember(&RecordType::entities, FieldName, ParentUUID, ChildUUID,
										  true);
		}

		inline bool DeleteEntity(field_name_t FieldName, const std::string &ParentUUID,
								 const std::string &ChildUUID) {
			return ManipulateVectorMember(&RecordType::entities, FieldName, ParentUUID, ChildUUID,
										  false);
		}

		inline bool AddUser(field_name_t FieldName, const std::string &ParentUUID,
							const std::string &ChildUUID) {
			return ManipulateVectorMember(&RecordType::users, FieldName, ParentUUID, ChildUUID,
										  true);
		}

		inline bool DelUser(field_name_t FieldName, const std::string &ParentUUID,
							const std::string &ChildUUID) {
			return ManipulateVectorMember(&RecordType::users, FieldName, ParentUUID, ChildUUID,
										  false);
		}

		inline bool AddConfiguration(field_name_t FieldName, const std::string &ParentUUID,
									 const std::string &ChildUUID) {
			return ManipulateVectorMember(&RecordType::deviceConfiguration, FieldName, ParentUUID,
										  ChildUUID, true);
		}

		inline bool DelConfiguration(field_name_t FieldName, const std::string &ParentUUID,
									 const std::string &ChildUUID) {
			return ManipulateVectorMember(&RecordType::deviceConfiguration, FieldName, ParentUUID,
										  ChildUUID, false);
		}

		inline bool AddVariable(field_name_t FieldName, const std::string &ParentUUID,
								const std::string &ChildUUID) {
			return ManipulateVectorMember(&RecordType::variables, FieldName, ParentUUID, ChildUUID,
										  true);
		}

		inline bool DelVariable(field_name_t FieldName, const std::string &ParentUUID,
								const std::string &ChildUUID) {
			return ManipulateVectorMember(&RecordType::variables, FieldName, ParentUUID, ChildUUID,
										  false);
		}

		inline bool AddInUse(field_name_t FieldName, const std::string &ParentUUID,
							 const std::string &Prefix, const std::string &ChildUUID) {
			std::string FakeUUID{Prefix + ":" + ChildUUID};
			return ManipulateVectorMember(&RecordType::inUse, FieldName, ParentUUID, FakeUUID,
										  true);
		}

		inline bool DeleteInUse(field_name_t FieldName, const std::string &ParentUUID,
								const std::string &Prefix, const std::string &ChildUUID) {
			std::string FakeUUID{Prefix + ":" + ChildUUID};
			return ManipulateVectorMember(&RecordType::inUse, FieldName, ParentUUID, FakeUUID,
										  false);
		}

		inline bool GetInUse(field_name_t FieldName, const std::string &UUID,
							 std::vector<std::string> &UUIDs) {
			RecordType R;
			if (GetRecord(FieldName, UUID, R)) {
				UUIDs = R.inUse;
				return true;
			}
			return false;
		}

		inline bool ValidFieldName(const std::string &FieldName) {
			return FieldNames_.find(Poco::toLower(FieldName)) != FieldNames_.end();
		}

		inline bool ValidFieldName(const char *FieldName) {
			std::string Field{FieldName};
			return ValidFieldName(Field);
		}

		[[nodiscard]] inline std::string ComputeRange(uint64_t From, uint64_t HowMany) {
			if (From < 1)
				From = 0;
			switch (Type_) {
			case OpenWifi::DBType::sqlite:
				return " LIMIT " + std::to_string(From) + ", " + std::to_string(HowMany) + " ";
			case OpenWifi::DBType::pgsql:
				return " LIMIT " + std::to_string(HowMany) + " OFFSET " + std::to_string(From) +
					   " ";
			case OpenWifi::DBType::mysql:
				return " LIMIT " + std::to_string(HowMany) + " OFFSET " + std::to_string(From) +
					   " ";
			default:
				return " LIMIT " + std::to_string(HowMany) + " OFFSET " + std::to_string(From) +
					   " ";
			}
		}

		Poco::Logger &Logger() { return Logger_; }

		inline bool DeleteRecordsFromCache(const char *FieldName, const std::string &Value) {
			if (Cache_)
				Cache_->Delete(FieldName, Value);
			return true;
		}

		inline void GetFieldNames(OpenWifi::Types::StringVec &F) {
			for (const auto &[field, _] : FieldNames_)
				F.push_back(field);
		}

		// Session-aware overloads for caller-owned transactions.
		// These methods do not acquire a session, manage the transaction,
		// or update Cache_. Cache changes must happen after a successful commit.

		template <typename T>
		bool GetRecord(Poco::Data::Session &session, field_name_t FieldName, const T &Value,
		               RecordType &R) {
			try {
				assert(ValidFieldName(FieldName));
				Poco::Data::Statement Select(session);
				RecordTuple RT;
				std::string St = "select " + SelectFields_ + " from " + TableName_ + " where " +
				                 FieldName + "=? limit 1";
				auto tValue{Value};
				Select << ConvertParams(St), Poco::Data::Keywords::into(RT),
				    Poco::Data::Keywords::use(tValue);
				if (Select.execute() == 1) {
					Convert(RT, R);
					return true;
				}
			} catch (const Poco::Exception &E) {
				Logger_.log(E);
			}
			return false;
		}

		bool GetRecords(Poco::Data::Session &session,
		                uint64_t Offset,
		                uint64_t HowMany,
		                RecordVec &Records,
		                const std::string &Where = "",
		                const std::string &OrderBy = "") {
			try {
				Poco::Data::Statement Select(session);
				RecordList RL;

				std::string St =
					"select " + SelectFields_ + " from " + TableName_ +
					(Where.empty() ? "" : " where " + Where) +
					OrderBy + ComputeRange(Offset, HowMany);

				Select << St, Poco::Data::Keywords::into(RL);
				Select.execute();

				if (Select.rowsExtracted() > 0) {
					for (auto &i : RL) {
						RecordType R;
						Convert(i, R);
						Records.emplace_back(R);
					}
					return true;
				}

				return false;
			} catch (const Poco::Exception &E) {
				Logger_.log(E);
			}

			return false;
		}

		// Transaction-aware read overloads for caller-owned transactions.
		// - 0 rows returned: normal "not found", returns false, transaction remains valid.
		// - DB/statement exception: returns false and marks tx failed, forcing rollback.

		template <typename T>
		bool GetRecord(OpenWifi::DbTransaction &tx, field_name_t FieldName, const T &Value,
		               RecordType &R) {
			try {
				assert(ValidFieldName(FieldName));
				Poco::Data::Statement Select(tx.Session());
				RecordTuple RT;
				std::string St = "select " + SelectFields_ + " from " + TableName_ + " where " + FieldName + "=? limit 1";
				auto tValue{Value};
				Select << ConvertParams(St), Poco::Data::Keywords::into(RT), Poco::Data::Keywords::use(tValue);
				if (Select.execute() == 1) {
					Convert(RT, R);
					return true;
				}
			} catch (const Poco::Exception &E) {
				Logger_.log(E);
				tx.MarkFailed();
			} catch (const std::exception &E) {
				Logger_.error("GetRecord failed: " + std::string(E.what()));
				tx.MarkFailed();
			} catch (...) {
				Logger_.error("GetRecord failed: unknown exception");
				tx.MarkFailed();
			}
			return false;
		}

		// SELECT ... FOR UPDATE row-level locking on caller transaction (PostgreSQL only, bypasses cache).
		template <typename T>
		bool GetRecordForUpdate(OpenWifi::DbTransaction &tx, field_name_t FieldName, const T &Value,
		                        RecordType &R) {
			if (Type_ != OpenWifi::DBType::pgsql) {
				Logger_.error("GetRecordForUpdate is only supported on PostgreSQL. "
					"SELECT ... FOR UPDATE on '" + TableName_ + "' was requested on a non-PostgreSQL backend. "
					"Call site must be guarded or migrated to PostgreSQL before using row-level locking.");
				tx.MarkFailed();
				return false;
			}
			try {
				assert(ValidFieldName(FieldName));
				// Cache_ is intentionally bypassed: a cached object cannot hold a PostgreSQL row lock.
				Poco::Data::Statement Select(tx.Session());
				RecordTuple RT;
				std::string St = "select " + SelectFields_ + " from " + TableName_ + " where " + FieldName + "=? limit 1 for update";
				auto tValue{Value};
				Select << ConvertParams(St), Poco::Data::Keywords::into(RT), Poco::Data::Keywords::use(tValue);
				if (Select.execute() == 1) {
					Convert(RT, R);
					return true;
				}
				// Row not found: not a transaction error. Caller decides how to handle.
			} catch (const Poco::Exception &E) {
				Logger_.log(E);
				tx.MarkFailed();
			} catch (const std::exception &E) {
				Logger_.error("GetRecordForUpdate failed: " + std::string(E.what()));
				tx.MarkFailed();
			} catch (...) {
				Logger_.error("GetRecordForUpdate failed: unknown exception");
				tx.MarkFailed();
			}
			return false;
		}

		bool GetRecords(OpenWifi::DbTransaction &tx,
		                uint64_t Offset,
		                uint64_t HowMany,
		                RecordVec &Records,
		                const std::string &Where = "",
		                const std::string &OrderBy = "") {
			try {
				Poco::Data::Statement Select(tx.Session());
				RecordList RL;
				std::string St = "select " + SelectFields_ + " from " + TableName_ + (Where.empty() ? "" : " where " + Where) + OrderBy + ComputeRange(Offset, HowMany);
				Select << St, Poco::Data::Keywords::into(RL);
				Select.execute();
				if (Select.rowsExtracted() > 0) {
					for (auto &i : RL) {
						RecordType R;
						Convert(i, R);
						Records.emplace_back(R);
					}
					return true;
				}
			} catch (const Poco::Exception &E) {
				Logger_.log(E);
				tx.MarkFailed();
			} catch (const std::exception &E) {
				Logger_.error("GetRecords failed: " + std::string(E.what()));
				tx.MarkFailed();
			} catch (...) {
				Logger_.error("GetRecords failed: unknown exception");
				tx.MarkFailed();
			}
			return false;
		}

		// Transaction-aware ORM operations for caller-owned transactions.
		// - Failing write operations mark tx failed so tx.Commit() enforces rollback.
		// - Writes perform post-commit cache invalidation rather than direct mutation.
		bool CreateRecord(OpenWifi::DbTransaction &tx, const RecordType &R,
		                  OpenWifi::DbTransaction::PostCommitFunc postCommitInvalidation = nullptr) {
			try {
				Poco::Data::Statement Insert(tx.Session());
				RecordTuple RT;
				Convert(R, RT);
				std::string St = "insert into  " + TableName_ + " ( " + SelectFields_ +
				                 " ) values " + SelectList_;
				Insert << ConvertParams(St), Poco::Data::Keywords::use(RT);
				Insert.execute();
				RegisterPostCommitInvalidation(tx, std::move(postCommitInvalidation));
				return true;
			} catch (const Poco::Exception &E) {
				Logger_.log(E);
				tx.MarkFailed();
			} catch (const std::exception &E) {
				Logger_.error("CreateRecord failed: " + std::string(E.what()));
				tx.MarkFailed();
			} catch (...) {
				Logger_.error("CreateRecord failed: unknown exception");
				tx.MarkFailed();
			}
			return false;
		}

		template <typename T>
		bool UpdateRecord(OpenWifi::DbTransaction &tx, field_name_t FieldName, const T &Value,
		                  const RecordType &R,
		                  OpenWifi::DbTransaction::PostCommitFunc postCommitInvalidation = nullptr) {
			try {
				assert(ValidFieldName(FieldName));
				Poco::Data::Statement Update(tx.Session());
				RecordTuple RT;
				Convert(R, RT);
				auto tValue(Value);
				std::string St = "update " + TableName_ + " set " + UpdateFields_ +
				                 " where " + FieldName + "=?";
				Update << ConvertParams(St), Poco::Data::Keywords::use(RT),
				    Poco::Data::Keywords::use(tValue);
				const auto AffectedRows = Update.execute();
				if (AffectedRows != 1) {
					Logger_.warning(
						"UpdateRecord affected " + std::to_string(AffectedRows) +
						" rows in table '" + TableName_ +
						"' for field '" + FieldName + "'."
					);
					tx.MarkFailed();
					return false;
				}
				RegisterPostCommitInvalidation(tx, std::move(postCommitInvalidation), FieldName, to_string(Value));
				return true;
			} catch (const Poco::Exception &E) {
				Logger_.log(E);
				tx.MarkFailed();
			} catch (const std::exception &E) {
				Logger_.error("UpdateRecord failed: " + std::string(E.what()));
				tx.MarkFailed();
			} catch (...) {
				Logger_.error("UpdateRecord failed: unknown exception");
				tx.MarkFailed();
			}
			return false;
		}

		template <typename T>
		bool DeleteRecord(OpenWifi::DbTransaction &tx, field_name_t FieldName, const T &Value,
		                  OpenWifi::DbTransaction::PostCommitFunc postCommitInvalidation = nullptr) {
			try {
				assert(ValidFieldName(FieldName));
				Poco::Data::Statement Delete(tx.Session());
				std::string St = "delete from " + TableName_ + " where " + FieldName + "=?";
				auto tValue{Value};
				Delete << ConvertParams(St), Poco::Data::Keywords::use(tValue);
				const auto AffectedRows = Delete.execute();
				if (AffectedRows != 1) {
					Logger_.warning(
						"DeleteRecord affected " + std::to_string(AffectedRows) +
						" rows in table '" + TableName_ +
						"' for field '" + FieldName + "'."
					);
					tx.MarkFailed();
					return false;
				}
				RegisterPostCommitInvalidation(tx, std::move(postCommitInvalidation), FieldName, to_string(Value));
				return true;
			} catch (const Poco::Exception &E) {
				Logger_.log(E);
				tx.MarkFailed();
			} catch (const std::exception &E) {
				Logger_.error("DeleteRecord failed: " + std::string(E.what()));
				tx.MarkFailed();
			} catch (...) {
				Logger_.error("DeleteRecord failed: unknown exception");
				tx.MarkFailed();
			}
			return false;
		}

		// Low-level unsafe transaction bulk deletion.
		// WARNING:
		// - WhereClause is interpolated directly into raw SQL without parameter binding.
		//   It MUST be constructed exclusively from trusted internal predicates.
		//   NEVER pass API or user-supplied text directly (SQL injection hazard).
		// - Caller MUST provide an explicit post-commit invalidation callback, as generic
		//   ORM cannot deduce which cached keys or entries are affected by arbitrary WHERE clauses.
		bool DeleteRecordsUnsafe(OpenWifi::DbTransaction &tx, const std::string &WhereClause, OpenWifi::DbTransaction::PostCommitFunc postCommitInvalidation) {
			if (!postCommitInvalidation) {
				Logger_.error("DeleteRecordsUnsafe called without required post-commit invalidation callback.");
				tx.MarkFailed();
				return false;
			}
			try {
				assert(!WhereClause.empty());
				Poco::Data::Statement Delete(tx.Session());
				std::string St = "delete from " + TableName_ + " where " + WhereClause;
				Delete << St;
				Delete.execute();
				tx.AfterCommit(std::move(postCommitInvalidation));
				return true;
			} catch (const Poco::Exception &E) {
				Logger_.log(E);
				tx.MarkFailed();
			} catch (const std::exception &E) {
				Logger_.error("DeleteRecordsUnsafe failed: " + std::string(E.what()));
				tx.MarkFailed();
			} catch (...) {
				Logger_.error("DeleteRecordsUnsafe failed: unknown exception");
				tx.MarkFailed();
			}
			return false;
		}

	  protected:
		std::string TableName_;
		OpenWifi::DBType Type_;
		Poco::Data::SessionPool &Pool_;
		Poco::Logger &Logger_;
		std::string Prefix_;
		DBCache<RecordType> *Cache_ = nullptr;

	  private:
		void InvalidateCacheEntry(const std::string &fieldName, const std::string &value) {
			if (!Cache_)
				return;

			try {
				Cache_->Delete(fieldName, value);
				return;
			} catch (const Poco::Exception &E) {
				Logger_.error("Cache entry invalidation failed: " + E.displayText());
			} catch (const std::exception &E) {
				Logger_.error("Cache entry invalidation failed: " + std::string(E.what()));
			} catch (...) {
				Logger_.error("Cache entry invalidation failed: unknown exception");
			}

			Logger_.error("Falling back to full cache invalidation after targeted invalidation failure.");
			InvalidateAllCache();
		}

		void InvalidateAllCache() {
			if (!Cache_)
				return;

			try {
				Cache_->InvalidateAll();
			} catch (const Poco::Exception &E) {
				Logger_.error("Full cache invalidation failed: " + E.displayText());
			} catch (const std::exception &E) {
				Logger_.error("Full cache invalidation failed: " + std::string(E.what()));
			} catch (...) {
				Logger_.error("Full cache invalidation failed: unknown exception");
			}
		}

		void RegisterPostCommitInvalidation(OpenWifi::DbTransaction &tx,
		                                    OpenWifi::DbTransaction::PostCommitFunc customCallback,
		                                    const std::string &fieldName = "",
		                                    const std::string &valStr = "") {
			if (customCallback) {
				tx.AfterCommit(std::move(customCallback));
			} else if (Cache_) {
				if (fieldName.empty()) {
					tx.AfterCommit([this]() { InvalidateAllCache(); });
				} else {
					tx.AfterCommit([this, fieldName, valStr]() {
						InvalidateCacheEntry(fieldName, valStr);
					});
				}
			}
		}

		std::string CreateFields_;
		std::string SelectFields_;
		std::string SelectList_;
		std::string UpdateFields_;
		std::vector<std::string> IndexCreation_;
		// Parallel to IndexCreation_ for PostgreSQL: retains the original ORM::Index
		// definition for each entry so Create() can validate surviving indexes.
		std::vector<ORM::Index> PgsqlIndexDefs_;
		std::map<std::string, int> FieldNames_;
		bool HasUnsupportedUniqueIndex_ = false;
	};
} // namespace ORM

