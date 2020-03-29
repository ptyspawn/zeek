// See the file "COPYING" in the main distribution directory for copyright.

#include "ScriptAnaly.h"
#include "DefItem.h"
#include "DefPoint.h"
#include "Desc.h"
#include "Expr.h"
#include "Stmt.h"
#include "Scope.h"
#include "Traverse.h"
#include "module_util.h"


static char obj_desc_storage[8192];

static const char* obj_desc(const BroObj* o)
	{
	ODesc d;
	o->Describe(&d);
	d.SP();
	o->GetLocationInfo()->Describe(&d);

	strcpy(obj_desc_storage, d.Description());

	return obj_desc_storage;
	}


typedef std::map<const ID*, DefinitionItem*> ID_to_DI_Map;

static DefinitionPoint no_def;

typedef std::map<const DefinitionItem*, DefinitionPoint> ReachingDefsMap;

class ReachingDefs {
public:
	void AddRDs(const ReachingDefs& rd)
		{
		auto& rd_m = rd.RDMap();

		for ( const auto& one_rd : rd_m )
			AddRD(one_rd.first, one_rd.second);
		}

	void AddRD(const DefinitionItem* di, DefinitionPoint dp)
		{
		rd_map.insert(ReachingDefsMap::value_type(di, dp));
		}

	bool HasDI(const DefinitionItem* di) const
		{
		return rd_map.find(di) != rd_map.end();
		}

	bool HasPair(const DefinitionItem* di, DefinitionPoint dp) const
		{
		auto l = rd_map.find(di);
		return l != rd_map.end() && l->second.SameAs(dp);
		}

	ReachingDefs Intersect(const ReachingDefs& r) const;
	ReachingDefs Union(const ReachingDefs& r) const;

	bool Differ(const ReachingDefs& r) const;

	void Dump() const;

	int Size() const	{ return rd_map.size(); }

protected:
	const ReachingDefsMap& RDMap() const	{ return rd_map; }

	void PrintRD(const DefinitionItem* di, const DefinitionPoint& dp) const;

	ReachingDefsMap rd_map;
};

ReachingDefs ReachingDefs::Intersect(const ReachingDefs& r) const
	{
	ReachingDefs res;

	auto i = rd_map.begin();
	while ( i != rd_map.end() )
		{
		if ( r.HasPair(i->first, i->second) )
			res.AddRD(i->first, i->second);

		++i;
		}

	return res;
	}

ReachingDefs ReachingDefs::Union(const ReachingDefs& r) const
	{
	ReachingDefs res = r;

	auto i = rd_map.begin();
	while ( i != rd_map.end() )
		{
		if ( ! r.HasPair(i->first, i->second) )
			res.AddRD(i->first, i->second);

		++i;
		}

	return res;
	}

bool ReachingDefs::Differ(const ReachingDefs& r) const
	{
	// This is just an optimization.
	if ( Size() != r.Size() )
		return false;

	auto res = Intersect(r);

	return res.Size() == Size();
	}

void ReachingDefs::Dump() const
	{
	if ( Size() == 0 )
		{
		printf("<none>\n");
		return;
		}

	for ( auto r = rd_map.begin(); r != rd_map.end(); ++r )
		PrintRD(r->first, r->second);
	}

void ReachingDefs::PrintRD(const DefinitionItem* di,
				const DefinitionPoint& dp) const
	{
	printf("RD for %s\n", di->Name());
	}

static ReachingDefs null_RDs;


typedef std::map<const BroObj*, ReachingDefs> AnalyInfo;

// Reaching definitions associated with a collection of BroObj's.
class ReachingDefSet {
public:
	ReachingDefSet(ID_to_DI_Map& _i2d_map) : i2d_map(_i2d_map)
		{
		a_i = new AnalyInfo;
		}

	~ReachingDefSet()
		{
		delete a_i;
		}

	bool HasRDs(const BroObj* o) const
		{
		auto RDs = a_i->find(o);
		return RDs != a_i->end();
		}

	bool HasRD(const BroObj* o, const ID* id) const
		{
		return HasRD(o, GetConstIDReachingDef(id));
		}

	bool HasRD(const BroObj* o, const DefinitionItem* di) const
		{
		auto RDs = a_i->find(o);
		if ( RDs == a_i->end() )
			return false;

		return RDs->second.HasDI(di);
		}

	const ReachingDefs& RDs(const BroObj* o) const
		{
		if ( o == nullptr )
			return null_RDs;

		auto rd = a_i->find(o);
		if ( rd != a_i->end() )
			return rd->second;
		else
			return null_RDs;
		}

	void AddRDs(const BroObj* o, const ReachingDefs& rd)
		{
		if ( HasRDs(o) )
			MergeRDs(o, rd);
		else
			a_i->insert(AnalyInfo::value_type(o, rd));
		}

	void AddRD(ReachingDefs& rd, const ID* id, DefinitionPoint dp);

	void AddRDWithInit(ReachingDefs& rd, const ID* id, DefinitionPoint dp,
				bool assume_full,const AssignExpr* init);

	void AddRDWithInit(ReachingDefs& rd, DefinitionItem* di,
				DefinitionPoint dp, bool assume_full,
				const AssignExpr* init);

	void CreateRecordRDs(ReachingDefs& rd, DefinitionItem* di,
				bool assume_full, DefinitionPoint dp,
				const DefinitionItem* rhs_di);

	// Gets definition for either a name or a record field reference.
	// Returns nil if "expr" lacks such a form, or if there isn't
	// any such definition.
	DefinitionItem* GetExprReachingDef(Expr* expr);

	DefinitionItem* GetIDReachingDef(const ID* id);
	const DefinitionItem* GetConstIDReachingDef(const ID* id) const;

	const DefinitionItem* GetConstIDReachingDef(const DefinitionItem* di,
						const char* field_name) const;

protected:
	void MergeRDs(const BroObj* o, const ReachingDefs& rd)
		{
		auto& curr_rds = a_i->find(o)->second;
		curr_rds.AddRDs(rd);
		}

	AnalyInfo* a_i;
	ID_to_DI_Map& i2d_map;
};

DefinitionItem* ReachingDefSet::GetIDReachingDef(const ID* id)
	{
	auto di = i2d_map.find(id);
	if ( di == i2d_map.end() )
		{
		auto new_entry = new DefinitionItem(id);
		i2d_map.insert(ID_to_DI_Map::value_type(id, new_entry));
		return new_entry;
		}
	else
		return di->second;
	}

const DefinitionItem* ReachingDefSet::GetConstIDReachingDef(const ID* id) const
	{
	auto di = i2d_map.find(id);
	if ( di != i2d_map.end() )
		return di->second;
	else
		return nullptr;
	}

const DefinitionItem* ReachingDefSet::GetConstIDReachingDef(const DefinitionItem* di,
					const char* field_name) const
	{
	return di->FindField(field_name);
	}

DefinitionItem* ReachingDefSet::GetExprReachingDef(Expr* expr)
	{
	if ( expr->Tag() == EXPR_NAME )
		{
		auto id_e = expr->AsNameExpr();
		auto id = id_e->Id();
		return GetIDReachingDef(id);
		}

	else if ( expr->Tag() == EXPR_FIELD )
		{
		auto f = expr->AsFieldExpr();
		auto r = f->Op();

		auto r_def = GetExprReachingDef(r);

		if ( ! r_def )
			return nullptr;

		auto field = f->FieldName();
		return r_def->FindField(field);
		}

	else
		return nullptr;
	}

void ReachingDefSet::AddRD(ReachingDefs& rd, const ID* id, DefinitionPoint dp)
	{
	if ( id == 0 )
		printf("oops\n");

	auto di = GetIDReachingDef(id);

	if ( di )
		rd.AddRD(di, dp);
	}

void ReachingDefSet::AddRDWithInit(ReachingDefs& rd, const ID* id,
				DefinitionPoint dp, bool assume_full,
				const AssignExpr* init)
	{
	auto di = GetIDReachingDef(id);
	if ( ! di )
		return;

	AddRDWithInit(rd, di, dp, assume_full, init);
	}

void ReachingDefSet::AddRDWithInit(ReachingDefs& rd, DefinitionItem* di,
				DefinitionPoint dp, bool assume_full,
				const AssignExpr* init)
	{
	rd.AddRD(di, dp);

	if ( di->Type()->Tag() != TYPE_RECORD )
		return;

	const DefinitionItem* rhs_di = nullptr;

	if ( init )
		{
		auto rhs = init->Op2();

		if ( rhs->Type()->Tag() == TYPE_ANY )
			// All bets are off.
			assume_full = true;

		else
			{
			rhs_di = GetExprReachingDef(rhs);

			if ( ! rhs_di )
				// This happens because the RHS is an
				// expression more complicated than just a
				// variable or a field reference.  Just assume
				// it's fully initialized.
				assume_full = true;
			}
		}

	CreateRecordRDs(rd, di, assume_full, dp, rhs_di);
	}

void ReachingDefSet::CreateRecordRDs(ReachingDefs& rd, DefinitionItem* di,
					bool assume_full, DefinitionPoint dp,
					const DefinitionItem* rhs_di)
	{
	// (1) deal with LHS record creators
	// (2) populate globals
	auto rt = di->Type()->AsRecordType();
	auto n = rt->NumFields();

	for ( auto i = 0; i < n; ++i )
		{
		auto n_i = rt->FieldName(i);
		auto rhs_di_i = rhs_di ? rhs_di->FindField(n_i) : nullptr;

		bool field_is_defined = false;

		if ( assume_full )
			field_is_defined = true;

		else if ( rhs_di_i )
			field_is_defined = true;

		else if ( rt->FieldHasAttr(i, ATTR_DEFAULT) )
			field_is_defined = true;

		if ( ! field_is_defined )
			continue;

		auto t_i = rt->FieldType(i);

		auto di_i = di->CreateField(n_i, t_i);
		rd.AddRD(di_i, dp);

		if ( t_i->Tag() == TYPE_RECORD )
			CreateRecordRDs(rd, di_i, assume_full, dp, rhs_di_i);
		}
	}


class RD_Decorate : public TraversalCallback {
public:
	RD_Decorate();
	~RD_Decorate() override;

	TraversalCode PreFunction(const Func*) override;
	TraversalCode PreStmt(const Stmt*) override;
	TraversalCode PostStmt(const Stmt*) override;
	TraversalCode PreExpr(const Expr*) override;
	TraversalCode PostExpr(const Expr*) override;

	void TrackInits(const Func* f, const id_list* inits);

protected:
	bool CheckLHS(ReachingDefs& rd, const Expr* lhs, const AssignExpr* a);

	bool IsAggrTag(TypeTag tag) const;
	bool IsAggr(const Expr* e) const;

	bool ControlReachesEnd(const Stmt* s, bool is_definite,
				bool ignore_break = false) const;

	const ReachingDefs& PredecessorRDs() const
		{
		auto& rd = PostRDs(last_obj);
		if ( rd.Size() > 0 )
			return rd;

		// PostRDs haven't been set yet.
		return PreRDs(last_obj);
		}

	const ReachingDefs& PreRDs(const BroObj* o) const
		{ return pre_defs->RDs(o); }
	const ReachingDefs& PostRDs(const BroObj* o) const
		{ return post_defs->RDs(o); }

	void AddPreRDs(const BroObj* o, const ReachingDefs& rd)
		{ pre_defs->AddRDs(o, rd); }
	void AddPostRDs(const BroObj* o, const ReachingDefs& rd)
		{ post_defs->AddRDs(o, rd); }

	bool HasPreRD(const BroObj* o, const ID* id) const
		{
		return pre_defs->HasRD(o, id);
		}

	// Mappings of reaching defs pre- and post- execution
	// of the given object.
	ReachingDefSet* pre_defs;
	ReachingDefSet* post_defs;

	// The object we most recently finished analyzing.
	const BroObj* last_obj;

	ID_to_DI_Map i2d_map;

	bool trace;
};


RD_Decorate::RD_Decorate()
	{
	pre_defs = new ReachingDefSet(i2d_map);
	post_defs = new ReachingDefSet(i2d_map);
	last_obj = nullptr;

	trace = getenv("ZEEK_OPT_TRACE") != nullptr;
	}

RD_Decorate::~RD_Decorate()
	{
	for ( auto& i2d : i2d_map )
		delete i2d.second;

	delete pre_defs;
	delete post_defs;
	}


TraversalCode RD_Decorate::PreFunction(const Func* f)
	{
	auto ft = f->FType();
	auto args = ft->Args();
	auto scope = f->GetScope();

	int n = args->NumFields();

	ReachingDefs rd;

	for ( int i = 0; i < n; ++i )
		{
		auto arg_i = args->FieldName(i);
		auto arg_i_id = scope->Lookup(arg_i);

		if ( ! arg_i_id )
			arg_i_id = scope->Lookup(make_full_var_name(current_module.c_str(), arg_i).c_str());

		post_defs->AddRDWithInit(rd, arg_i_id, DefinitionPoint(f),
						true, 0);
		}

	AddPostRDs(f, rd);
	last_obj = f;

	if ( trace )
		{
		printf("traversing function %s, post RDs:\n", f->Name());
		PostRDs(f).Dump();
		}

	// Don't continue traversal here, as that will then loop over
	// older bodies.  Instead, we do it manually.
	return TC_ABORTALL;
	}

TraversalCode RD_Decorate::PreStmt(const Stmt* s)
	{
	auto rd = PredecessorRDs();
	AddPreRDs(s, rd);

	rd = PreRDs(s);

	if ( trace )
		{
		printf("pre RDs for stmt %s:\n", stmt_name(s->Tag()));
		rd.Dump();
		}

	last_obj = s;

	switch ( s->Tag() ) {
	case STMT_IF:
		{
		// For now we assume there no definitions occur
		// inside the conditional.  If one does, we'll
		// detect that & complain about it in the PostStmt.
		auto i = s->AsIfStmt();

		// ### need to manually control traversal since
		// don't want RDs coming out of the TrueBranch
		// to propagate to the FalseBranch.
		auto my_rds = rd;
		AddPreRDs(i->TrueBranch(), my_rds);
		AddPreRDs(i->FalseBranch(), my_rds);

		break;
		}

	case STMT_SWITCH:
		{
		auto sw = s->AsSwitchStmt();
		auto cases = sw->Cases();

		for ( const auto& c : *cases )
			{
			auto type_ids = c->TypeCases();
			if ( type_ids )
				{
				for ( const auto& id : *type_ids )
					pre_defs->AddRDWithInit(rd, id,
						DefinitionPoint(s), true, 0);
				}

			AddPreRDs(c->Body(), rd);
			}

		break;
		}

	case STMT_FOR:
		{
		auto f = s->AsForStmt();

		auto ids = f->LoopVar();
		auto e = f->LoopExpr();
		auto body = f->LoopBody();

		for ( const auto& id : *ids )
			pre_defs->AddRDWithInit(rd, id, DefinitionPoint(s),
						true, 0);

		auto val_var = f->ValueVar();
		if ( val_var )
			pre_defs->AddRDWithInit(rd, val_var,
						DefinitionPoint(s), true, 0);

		AddPreRDs(e, rd);
		AddPreRDs(body, rd);

		if ( e->Tag() == EXPR_NAME )
			{
			// Don't traverse into the loop expression,
			// as it's okay if it's not initialized at this
			// point - that will just result in any empty loop.
			//
			// But then we do need to manually traverse the
			// body.
			body->Traverse(this);
			return TC_ABORTSTMT;

			// ### need to do PostStmt for For here
			}

		break;
		}

	case STMT_RETURN:
		{
		auto r = s->AsReturnStmt();
		auto e = r->StmtExpr();

		if ( e && IsAggr(e) )
			return TC_ABORTSTMT;

		break;
		}

	case STMT_ADD:
		{
		auto a = s->AsAddStmt();
		auto a_e = a->StmtExpr();

		if ( a_e->Tag() == EXPR_INDEX )
			{
			auto a_e_i = a_e->AsIndexExpr();
			auto a1 = a_e_i->Op1();
			auto a2 = a_e_i->Op2();

			if ( IsAggr(a1) )
				{
				a2->Traverse(this);

				auto i1 = a1->AsNameExpr()->Id();
				post_defs->AddRD(rd, i1, DefinitionPoint(s));
				AddPostRDs(s, rd);

				return TC_ABORTSTMT;
				}
			}

		break;
		}

	default:
		break;
	}

	return TC_CONTINUE;
	}

TraversalCode RD_Decorate::PostStmt(const Stmt* s)
	{
	ReachingDefs post_rds;

	switch ( s->Tag() ) {
	case STMT_PRINT:
	case STMT_EVENT:
	case STMT_WHEN:
		post_rds = PreRDs(s);
		break;

        case STMT_EXPR:
		{
		auto e = s->AsExprStmt()->StmtExpr();
		post_rds = PostRDs(e);
		break;
		}

	case STMT_IF:
		{
		auto i = s->AsIfStmt();

		if ( PostRDs(i).Differ(PreRDs(s)) )
			; // Complain

		auto if_branch_rd = PostRDs(i->TrueBranch());
		auto else_branch_rd = PostRDs(i->FalseBranch());

		auto true_reached = ControlReachesEnd(i->TrueBranch(), false);
		auto false_reached = ControlReachesEnd(i->FalseBranch(), false);

		if ( true_reached && false_reached )
			post_rds = if_branch_rd.Intersect(else_branch_rd);

		else if ( true_reached )
			post_rds = if_branch_rd;

		else if ( false_reached )
			post_rds = else_branch_rd;

		else
			; // leave empty

		break;
		}

	case STMT_SWITCH:
		{
		auto sw = s->AsSwitchStmt();
		auto cases = sw->Cases();

		bool did_first = false;
		bool default_seen = false;

		for ( const auto& c : *cases )
			{
			if ( ControlReachesEnd(c->Body(), false) )
				{
				auto case_rd = PostRDs(c->Body());
				if ( did_first )
					post_rds = post_rds.Intersect(case_rd);
				else
					post_rds = case_rd;
				}

			if ( (! c->ExprCases() ||
			      c->ExprCases()->Exprs().length() == 0) &&
			     (! c->TypeCases() ||
			      c->TypeCases()->length() == 0) )
				default_seen = true;
			}

		if ( ! default_seen )
			post_rds = post_rds.Union(PreRDs(s));

		break;
		}

	case STMT_FOR:
		{
		auto f = s->AsForStmt();
		auto body = f->LoopBody();

		// ### If post differs from pre, propagate to
		// beginning and re-traverse.

		// Apply intersection since loop might not execute
		// at all.
		post_rds = PreRDs(s).Intersect(PostRDs(body));

		break;
		}

	case STMT_WHILE:
		{
		auto w = s->AsWhileStmt();
		auto body = w->Body();

		// ### If post differs from pre, propagate to
		// beginning and re-traverse.

		// Apply intersection since loop might not execute
		// at all.
		post_rds = PreRDs(s).Intersect(PostRDs(body));

		break;
		}

	case STMT_LIST:
	case STMT_EVENT_BODY_LIST:
		{
		auto l = s->AsStmtList();
		auto stmts = l->Stmts();

		if ( ControlReachesEnd(l, false ) )
			{
			if ( stmts.length() == 0 )
				post_rds = PreRDs(s);
			else
				post_rds = PostRDs(stmts[stmts.length() - 1]);
			}

		else
			;  // leave empty

		break;
		}

	case STMT_INIT:
		{
		auto init = s->AsInitStmt();
		auto& inits = *init->Inits();

		post_rds = PreRDs(s);

		for ( int i = 0; i < inits.length(); ++i )
			{
			auto id = inits[i];
			auto id_t = id->Type();

			// Only aggregates get initialized.
			auto tag = id_t->Tag();
			if ( ! IsAggrTag(tag) )
				continue;

			post_defs->AddRDWithInit(post_rds, id,
						DefinitionPoint(s), false, 0);
			}

		break;
		}

	case STMT_NEXT:
	case STMT_BREAK:
	case STMT_RETURN:
		// No control flow past these statements, so no
		// post reaching defs.
		break;

	case STMT_FALLTHROUGH:
		// Yuck, really ought to propagate its RDs into
		// the next case, but that's quite ugly.  It
		// only matters if (1) there are meaningful
		// definitions crossing into the case *and*
		// (2) we start doing analyses that depend on
		// potential RDs and not just minimalist RDs.
		//
		// Anyhoo, punt for now. ###
		break;

	case STMT_ADD:
		// Tracking what's added to sets could have
		// some analysis utility but seems pretty rare,
		// so we punt for now. ###
		break;

	case STMT_DELETE:
		// Ideally we'd track these for removing optional
		// record elements, or (maybe) some inferences
		// about table/set elements. ###
		break;

	default:
		break;
	}

	AddPostRDs(s, post_rds);
	last_obj = s;

	if ( trace )
		{
		printf("post RDs for stmt %s:\n", stmt_name(s->Tag()));
		PostRDs(s).Dump();
		}

	return TC_CONTINUE;
	}

bool RD_Decorate::CheckLHS(ReachingDefs& rd, const Expr* lhs,
				const AssignExpr* a)
	{
	switch ( lhs->Tag() ) {
	case EXPR_REF:
		{
		auto r = lhs->AsRefExpr();
		return CheckLHS(rd, r->Op(), a);
		}

	case EXPR_NAME:
		{
		auto n = lhs->AsNameExpr();
		auto id = n->Id();

		post_defs->AddRDWithInit(rd, id, DefinitionPoint(a), false, a);

		return true;
		}

	case EXPR_LIST:
		{
		auto l = lhs->AsListExpr();
		for ( const auto& expr : l->Exprs() )
			{
			if ( expr->Tag() != EXPR_NAME )
				// This will happen for table initialiers,
				// for example.
				return false;

			auto n = expr->AsNameExpr();
			auto id = n->Id();

			// Since the typing on the RHS may be dynamic,
			// we don't try to do any inference of possible
			// missing fields, hence "true" in the following.
			post_defs->AddRDWithInit(rd, id, DefinitionPoint(a),
						true, 0);
			}

		return true;
		}

        case EXPR_FIELD:
		{
		auto f = lhs->AsFieldExpr();
		auto r = f->Op();

		if ( r->Tag() != EXPR_NAME && r->Tag() != EXPR_FIELD )
			// This is a more complicated expression that we're
			// not able to concretely track.
			return false;

		// Recurse to traverse LHS so as to install its definitions.
		r->Traverse(this);

		auto r_def = post_defs->GetExprReachingDef(r);

		if ( ! r_def )
			// This should have already generated a complaint.
			// Avoid cascade.
			return true;

		auto fn = f->FieldName();

		auto field_rd = r_def->FindField(fn);
		auto ft = f->Type();
		if ( ! field_rd )
			field_rd = r_def->CreateField(fn, ft);

		post_defs->AddRDWithInit(rd, field_rd, DefinitionPoint(a),
						false, a);

		return true;
		}

        case EXPR_INDEX:
		{
		auto i_e = lhs->AsIndexExpr();
		auto aggr = i_e->Op1();
		auto index = i_e->Op2();

		if ( aggr->Tag() == EXPR_NAME )
			{
			// Count this as an initialization of the aggregate.
			auto id = aggr->AsNameExpr()->Id();
			pre_defs->AddRD(rd, id, DefinitionPoint(a));

			// Don't recurse into assessing the aggregate,
			// since it's okay in this context.  However,
			// we do need to recurse into the index, which
			// could have problems.
			index->Traverse(this);
			return true;
			}

		return false;
		}

	default:
		return false;
	}
	}

bool RD_Decorate::IsAggrTag(TypeTag tag) const
	{
	return tag == TYPE_VECTOR || tag == TYPE_TABLE || tag == TYPE_RECORD;
	}

bool RD_Decorate::IsAggr(const Expr* e) const
	{
	if ( e->Tag() != EXPR_NAME )
		return false;

	auto n = e->AsNameExpr();
	auto id = n->Id();
	auto tag = id->Type()->Tag();

	return IsAggrTag(tag);
	}

bool RD_Decorate::ControlReachesEnd(const Stmt* s, bool is_definite,
					bool ignore_break) const
	{
	switch ( s->Tag() ) {
	case STMT_NEXT:
	case STMT_RETURN:
		return false;

	case STMT_BREAK:
		return ignore_break;

	case STMT_IF:
		{
		auto i = s->AsIfStmt();

		auto true_reaches =
			ControlReachesEnd(i->TrueBranch(), is_definite);
		auto false_reaches =
			ControlReachesEnd(i->FalseBranch(), is_definite);

		if ( is_definite )
			return true_reaches && false_reaches;
		else
			return true_reaches || false_reaches;
		}

	case STMT_SWITCH:
		{
		auto sw = s->AsSwitchStmt();
		auto cases = sw->Cases();

		bool control_reaches_end = is_definite;
		bool default_seen = false;
		for ( const auto& c : *cases )
			{
			bool body_def = ControlReachesEnd(c->Body(),
								is_definite,
								true);

			if ( is_definite && ! body_def )
				control_reaches_end = false;

			if ( ! is_definite && body_def )
				control_reaches_end = true;

			if ( (! c->ExprCases() ||
			      c->ExprCases()->Exprs().length() == 0) &&
			     (! c->TypeCases() ||
			      c->TypeCases()->length() == 0) )
				default_seen = true;
			}

		if ( ! is_definite && ! default_seen )
			return true;

		return control_reaches_end;
		}

	case STMT_LIST:
	case STMT_EVENT_BODY_LIST:
		{
		auto l = s->AsStmtList();

		bool reaches_so_far = true;

		for ( const auto& stmt : l->Stmts() )
			{
			if ( ! reaches_so_far )
				{
				printf("dead code: %s\n", obj_desc(stmt));
				return false;
				}

			if ( ! ControlReachesEnd(stmt, is_definite,
							ignore_break) )
				reaches_so_far = false;
			}

		return reaches_so_far;
		}

	default:
		return true;
	}
	}

TraversalCode RD_Decorate::PreExpr(const Expr* e)
	{
	auto rd = PredecessorRDs();
	AddPreRDs(e, rd);

	if ( trace )
		{
		printf("pre RDs for expr %s:\n", expr_name(e->Tag()));
		PreRDs(e).Dump();
		}

	last_obj = e;

	switch ( e->Tag() ) {
        case EXPR_NAME:
		{
		auto n = e->AsNameExpr();
		auto id = n->Id();

		if ( id->IsGlobal() )
			{
			// Treat global as fully initialized.
			pre_defs->AddRDWithInit(rd, id, DefinitionPoint(n),
							true, nullptr);
			AddPreRDs(e, rd);
			}

		if ( ! HasPreRD(e, id) )
			printf("%s has no pre at %s\n", id->Name(), obj_desc(e));

		if ( id->Type()->Tag() == TYPE_RECORD )
			{
			post_defs->CreateRecordRDs(rd, post_defs->GetIDReachingDef(id),
					false, DefinitionPoint(n), nullptr);
			AddPostRDs(e, rd);
			}

		break;
		}

        case EXPR_ADD_TO:
		{
		auto a_t = e->AsAddToExpr();
		auto lhs = a_t->Op1();

		if ( IsAggr(lhs) )
			{
			auto lhs_n = lhs->AsNameExpr();
			auto lhs_id = lhs_n->Id();

			// Treat this as an initalization of the set.
			post_defs->AddRD(rd, lhs_id, DefinitionPoint(a_t));
			AddPostRDs(e, PreRDs(e));
			AddPostRDs(e, rd);

			a_t->Op2()->Traverse(this);
			return TC_ABORTSTMT;
			}

		break;
		}

        case EXPR_ASSIGN:
		{
		auto a = e->AsAssignExpr();
		auto lhs = a->Op1();
		auto rhs = a->Op2();

		bool rhs_aggr = IsAggr(rhs);

		if ( CheckLHS(rd, lhs, a) )
			{
			AddPostRDs(e, PreRDs(e));
			AddPostRDs(e, rd);

			if ( ! rhs_aggr )
				rhs->Traverse(this);

			return TC_ABORTSTMT;
			}

		if ( rhs_aggr )
			{
			// No need to analyze the RHS.
			lhs->Traverse(this);
			return TC_ABORTSTMT;
			}

		// Too hard to figure out what's going on with the assignment.
		// Just analyze it in terms of values it accesses.
		break;
		}

	case EXPR_FIELD:
		{
		auto f = e->AsFieldExpr();
		auto r = f->Op();

		if ( r->Tag() != EXPR_NAME && r->Tag() != EXPR_FIELD )
			break;

		r->Traverse(this);
		auto r_def = pre_defs->GetExprReachingDef(r);

		if ( r_def )
			{
			auto fn = f->FieldName();
			auto field_rd =
				pre_defs->GetConstIDReachingDef(r_def, fn);

			if ( ! field_rd )
				printf("no reaching def for %s\n", obj_desc(e));
			}

		return TC_ABORTSTMT;
		}

	case EXPR_HAS_FIELD:
		{
		auto hf = e->AsHasFieldExpr();
		auto r = hf->Op();

		// Treat this as a definition of lhs$fn, since it's
		// assuring that that field exists.

		if ( r->Tag() == EXPR_NAME )
			{
			auto id_e = r->AsNameExpr();
			auto id = id_e->Id();
			auto id_rt = id_e->Type()->AsRecordType();
			auto id_rd = post_defs->GetIDReachingDef(id);

			if ( ! id_rd )
				printf("no ID reaching def for %s\n", id->Name());

			auto fn = hf->FieldName();
			auto field_rd = id_rd->FindField(fn);
			if ( ! field_rd )
				{
				auto ft = id_rt->FieldType(fn);
				field_rd = id_rd->CreateField(fn, ft);
				rd.AddRD(field_rd, DefinitionPoint(hf));
				AddPostRDs(e, rd);
				}
			}

		break;
		}

	case EXPR_CALL:
		{
		auto c = e->AsCallExpr();
		auto f = c->Func();
		auto args_l = c->Args();

		// If one of the arguments is an aggregate, then
		// it's actually passed by reference, and we shouldn't
		// ding it for not being initialized.
		//
		// We handle this by just doing the traversal ourselves.
		f->Traverse(this);

		for ( const auto& expr : args_l->Exprs() )
			{
			if ( IsAggr(expr) )
				// Not only do we skip analyzing it, but
				// we consider it initialized post-return.
				post_defs->AddRD(rd, expr->AsNameExpr()->Id(), 
					DefinitionPoint(c));
			else
				expr->Traverse(this);
			}

		AddPostRDs(e, PreRDs(e));
		AddPostRDs(e, rd);

		return TC_ABORTSTMT;
		}

	case EXPR_LAMBDA:
		// ### Too tricky to get these right.
		AddPostRDs(e, PreRDs(e));
		return TC_ABORTSTMT;

	default:
		break;
	}

	AddPostRDs(e, PreRDs(e));

	return TC_CONTINUE;
	}

TraversalCode RD_Decorate::PostExpr(const Expr* e)
	{
	AddPostRDs(e, PreRDs(e));
	return TC_CONTINUE;
	}

void RD_Decorate::TrackInits(const Func* f, const id_list* inits)
	{
	// This code is duplicated for STMT_INIT.  It's a pity that
	// that doesn't get used for aggregates that are initialized
	// just incidentally.
	ReachingDefs rd;
	for ( int i = 0; i < inits->length(); ++i )
		{
		auto id = (*inits)[i];
		auto id_t = id->Type();

		// Only aggregates get initialized.
		auto tag = id_t->Tag();
		if ( IsAggrTag(tag) )
			post_defs->AddRDWithInit(rd, id, DefinitionPoint(f),
							false, 0);
		}

	AddPostRDs(f, rd);
	}


class FolderFinder : public TraversalCallback {
public:
	// TraversalCode PreExpr(const Expr*) override;
	TraversalCode PreExpr(const Expr*, const Expr*) override;
	TraversalCode PreExpr(const Expr*, const Expr*, const Expr*) override;

protected:
	void ReportFoldable(const Expr* e, const char* type);
};

void FolderFinder::ReportFoldable(const Expr* e, const char* type)
	{
	printf("foldable %s: %s\n", type, obj_desc(e));
	}

TraversalCode FolderFinder::PreExpr(const Expr* expr, const Expr* op)
	{
	if ( op->IsConst() )
		ReportFoldable(expr, "unary");

	return TC_CONTINUE;
	}

TraversalCode FolderFinder::PreExpr(const Expr* expr, const Expr* op1, const Expr* op2)
	{
	if ( op1->IsConst() && op2->IsConst() )
		ReportFoldable(expr, "binary");

	return TC_CONTINUE;
	}


bool did_init = false;
bool activate = false;
const char* only_func = 0;

void analyze_func(const Func* f, const id_list* inits, const Stmt* body)
	{
	if ( ! did_init )
		{
		if ( getenv("ZEEK_ANALY") )
			activate = true;

		only_func = getenv("ZEEK_ONLY");

		if ( only_func )
			activate = true;

		did_init = true;
		}

	if ( ! activate )
		return;

	if ( ! only_func || streq(f->Name(), only_func) )
		{
		RD_Decorate cb;
		f->Traverse(&cb);
		cb.TrackInits(f, inits);
		body->Traverse(&cb);
		}
	}
