/* <flow/FlowRunner.cpp>
 *
 * This file is part of the x0 web server project and is released under LGPL-3.
 * http://redmine.xzero.ws/projects/flow
 *
 * (c) 2010 Christian Parpart <trapni@gentoo.org>
 */

#include <x0/flow/FlowRunner.h>
#include <x0/flow/FlowParser.h>
#include <x0/flow/FlowBackend.h>
#include <x0/IPAddress.h>
#include <x0/RegExp.h>

#include <llvm/DerivedTypes.h>
#include <llvm/LLVMContext.h>
#include <llvm/Module.h>
#include <llvm/LLVMContext.h>
#include <llvm/Analysis/Verifier.h>
#include <llvm/Support/IRBuilder.h>
#include <llvm/Support/StandardPasses.h>
#include <llvm/Support/ManagedStatic.h>
#include <llvm/PassManager.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/JIT.h>
#include <llvm/Target/TargetSelect.h>
#include <llvm/Target/TargetData.h>

#include <fstream>

#define FLOW_PCRE (1) /* TODO make this configurable via cmake ! */

#if defined(FLOW_PCRE)
#	include <pcre.h>
#endif

// TODO
// - IR equivalent to strcasestr for cstring/nbuf args (len1, buf1, len2, buf2)

namespace x0 {

#if 0
// {{{ trace
static size_t fnd = 0;
struct fntrace {
	std::string msg_;

	fntrace(const char *msg) : msg_(msg)
	{
		++fnd;

		for (size_t i = 0; i < fnd; ++i)
			printf("  ");

		printf("-> %s\n", msg_.c_str());
	}

	~fntrace() {
		for (size_t i = 0; i < fnd; ++i)
			printf("  ");

		--fnd;

		printf("<- %s\n", msg_.c_str());
	}
};
// }}}
#	define FNTRACE() fntrace _(__PRETTY_FUNCTION__)
#	define TRACE(msg...) do { printf(msg); printf("\n"); } while (0)
#else
#	define FNTRACE() /*!*/
#	define TRACE(msg...) /*!*/
#endif

// {{{ LLVM helper methods
/** tests whether given type *COULD* be a string type (i8*).
  */
static inline bool isBoolTy(const llvm::Type *type)
{
	if (!type->isIntegerTy())
		return false;

	const llvm::IntegerType *i = static_cast<const llvm::IntegerType *>(type);
	if (i->getBitWidth() != 1)
		return false;

	return true;
}

static inline bool isBool(const llvm::Value *value)
{
	return isBoolTy(value->getType());
}
// }}}

// {{{ FlowRunner::Scope
FlowRunner::Scope::Scope()
{
	enter(); // global scope
}

FlowRunner::Scope::~Scope()
{
	while (scope_.size())
		leave();
}

void FlowRunner::Scope::clear()
{
	// pop all scopes
	while (scope_.size())
		leave();

	// re-enter new global scope
	enter();
}

void FlowRunner::Scope::enter()
{
	scope_.push_front(new std::map<Symbol *, llvm::Value *>());
}

void FlowRunner::Scope::leave()
{
	delete scope_.front();
	scope_.pop_front();
}

llvm::Value *FlowRunner::Scope::lookup(Symbol *symbol) const
{
	for (auto i: scope_) {
		auto k = i->find(symbol);

		if (k != i->end()) {
			return k->second;
		}
	}

	return NULL;
}

void FlowRunner::Scope::insert(Symbol *symbol, llvm::Value *value)
{
	(*scope_.front())[symbol] = value;
}

void FlowRunner::Scope::insertGlobal(Symbol *symbol, llvm::Value *value)
{
	(*scope_.back())[symbol] = value;
}

void FlowRunner::Scope::remove(Symbol *symbol)
{
	auto i = scope_.front()->find(symbol);

	if (i != scope_.front()->end())
		scope_.front()->erase(i);
}
// }}}

FlowRunner::FlowRunner(FlowBackend *b) :
	backend_(b),
	parser_(new FlowParser(b)),
	unit_(NULL),
	optimizationLevel_(0),
	errorHandler_(),
	cx_(),
	module_(NULL),
	valueType_(NULL),
	regexpType_(NULL),
	bufferType_(NULL),
	builder_(cx_),
	value_(NULL),
	initializerFn_(NULL),
	initializerBB_(NULL),
	scope_(),
	requestingLvalue_(false),
	functionPassMgr_(NULL),
	executionEngine_(NULL)
{
	llvm::InitializeNativeTarget();
	//cx_ = llvm::getGlobalContext();

	std::memset(coreFunctions_, 0, sizeof(coreFunctions_));

	reinitialize();
}

void FlowRunner::initialize()
{
	llvm::InitializeNativeTarget();
}

void FlowRunner::shutdown()
{
	llvm::llvm_shutdown();
}

FlowRunner::~FlowRunner()
{
	clear();

	delete parser_;
	parser_ = nullptr;
}

int FlowRunner::optimizationLevel() const
{
	return optimizationLevel_;
}

void FlowRunner::setOptimizationLevel(int value)
{
	optimizationLevel_ = std::min(std::max(value, 0), 4);

	if (functionPassMgr_)
		delete functionPassMgr_;

	functionPassMgr_ = new llvm::FunctionPassManager(module_);
	functionPassMgr_->add(new llvm::TargetData(*executionEngine_->getTargetData()));

	if (optimizationLevel_)
		llvm::createStandardFunctionPasses(functionPassMgr_, optimizationLevel_);
}

void FlowRunner::clear()
{
	// freeing machine code for each function generated code for
	for (auto i: functions_)
		executionEngine_->freeMachineCodeForFunction(i);
	functions_.clear();

	value_ = NULL;
	scope_.clear();

	delete functionPassMgr_;
	functionPassMgr_ = NULL;

	delete executionEngine_;
	executionEngine_ = NULL;

	//FIXME crash: delete module_;
}

bool FlowRunner::reinitialize()
{
	assert(module_ == NULL);
	assert(executionEngine_ == NULL);

	// create LLVM module to put all our generated code in to
	module_ = new llvm::Module("flow", cx_);

	// create JITting execution engine
	std::string errorStr;
	executionEngine_ = llvm::EngineBuilder(module_).setErrorStr(&errorStr).create();
	if (!executionEngine_) {
		printf("execution engine creation failed. %s\n", errorStr.c_str());
		return false;
	}

	// create generatic native-value type, for exchanging parameter/return values
	std::vector<const llvm::Type *> elts;
	elts.push_back(int32Type());     // type id
	elts.push_back(numberType());    // number (long long)
	elts.push_back(int8PtrType());   // string (char*)
	valueType_ = llvm::StructType::get(cx_, elts, true/*packed*/);
	module_->addTypeName("nval", valueType_);

	elts.clear();
	elts.push_back(int8PtrType());   // name (const char *)
	elts.push_back(int8PtrType());   // handle (pcre *)
	regexpType_ = llvm::StructType::get(cx_, elts, true/*packed*/);
	module_->addTypeName("regexp", regexpType_);

	elts.clear();
	elts.push_back(int32Type());	// domain (AF_INET, AF_INET6)
	elts.push_back(int32Type());
	elts.push_back(int32Type());
	elts.push_back(int32Type());
	elts.push_back(int32Type());
	ipaddrType_ = llvm::StructType::get(cx_, elts, true/*packed*/);

	elts.clear();
	elts.push_back(int64Type());     // buffer length
	elts.push_back(int8PtrType());   // buffer data
	bufferType_ = llvm::StructType::get(cx_, elts, true/*packed*/);
	module_->addTypeName("nbuf", bufferType_);

	// declare native callback signatures
	emitNativeFunctionSignature();
	emitCoreFunctions();

	return true;
}

void FlowRunner::reset()
{
	clear();
	reinitialize();
}

void FlowRunner::dump(const char *msg)
{
	if (msg) {
		printf("-------------------------------------------------\n");
		printf("-- %s:\n", msg);
	}

	module_->dump();

	if (msg)
		printf("-------------------------------------------------\n");
}

void FlowRunner::setErrorHandler(std::function<void(const std::string&)>&& callback)
{
	errorHandler_ = callback;
}

void FlowRunner::reportError(const std::string& message)
{
	if (errorHandler_)
	{
		char buf[1024];
		snprintf(buf, sizeof(buf), "code generator error: %s", message.c_str());

		errorHandler_(buf);
	}
	value_ = NULL;
}

bool FlowRunner::open(const std::string& filename)
{
	// parse source
	std::fstream fs(filename);
	if (!parser_->initialize(&fs)) {
		perror("open");
		return false;
	}

	unit_ = parser_->parse();

	if (!unit_)
		return false;

	// generate machine code
	if (executionEngine_ == NULL)
		if (!reinitialize())
			return false;

	codegen(unit_);

	if (HandlerFunction init = reinterpret_cast<HandlerFunction>(executionEngine_->getPointerToFunction(initializerFn_)))
		init(NULL);

	return true;
}

void FlowRunner::close()
{
	// XXX think about introducing a finalizerFn

	clear();// XXX !

	delete unit_;
	unit_ = nullptr;
}

std::vector<Function*> FlowRunner::getHandlerList() const
{
	std::vector<Function*> result;

	for (std::size_t i = 0, e = unit_->length(); i != e; ++i) {
		Symbol *sym = unit_->at(i);
		if (!sym->isFunction())
			continue;

		Function *fn = static_cast<Function*>(sym);
		if (!fn->isHandler())
			continue;

		result.push_back(fn);
	}

	return result;
}

Function *FlowRunner::findHandler(const std::string& name) const
{
	for (std::size_t i = 0, e = unit_->length(); i != e; ++i) {
		Symbol *sym = unit_->at(i);
		if (!sym->isFunction())
			continue;

		Function *fn = static_cast<Function*>(sym);
		if (!fn->isHandler())
			continue;

		if (fn->name() != name)
			continue;

		return fn;
	}

	return nullptr;
}

FlowRunner::HandlerFunction FlowRunner::getPointerTo(Function *handler)
{
	assert(handler != nullptr);
	assert(executionEngine_ != nullptr);

	llvm::Function *fn = codegen<llvm::Function>(handler);
	if (!fn) {
		printf("function IR generation failed\n");
		return nullptr;
	}

	return reinterpret_cast<HandlerFunction>(executionEngine_->getPointerToFunction(fn));
}

bool FlowRunner::invoke(Function* handler, void* data)
{
	HandlerFunction fp = getPointerTo(handler);
	if (fp)
		return fp(data);

	return false;
}

llvm::Value *FlowRunner::codegen(Symbol *symbol)
{
	FNTRACE();
	TRACE("symbol: '%s'", symbol->name().c_str());

	if (llvm::Value *v = scope_.lookup(symbol))
		return value_ = v;

	auto c1 = builder_.GetInsertBlock();

	if (symbol)
		symbol->accept(*this);

	auto c2 = builder_.GetInsertBlock();

	if (c1 && c2 && c1->getParent() != c2->getParent())
		module_->dump();
	assert((!c1 && !c2) || (c1 && c2 && c1->getParent() == c2->getParent()));

	return value_;
}

llvm::Value *FlowRunner::codegen(Expr *expr)
{
	FNTRACE();

	auto c1 = builder_.GetInsertBlock();

	if (expr)
		expr->accept(*this);

	auto c2 = builder_.GetInsertBlock();
	assert(c1->getParent() == c2->getParent());

	return value_;
}

/** generates code for the following statement.
 */
void FlowRunner::codegen(Stmt *stmt)
{
	FNTRACE();

	auto c1 = builder_.GetInsertBlock();

	if (stmt)
		stmt->accept(*this);

	auto c2 = builder_.GetInsertBlock();
	assert(c1->getParent() == c2->getParent());
}

// {{{ backend glue
int FlowRunner::findNative(const std::string& name) const
{
	return backend_->find(name);
}
// }}}

// {{{ standard types
const Type *FlowRunner::stringType() const
{
	return llvm::Type::getInt8PtrTy(cx_);
}

const Type *FlowRunner::numberType() const
{
	return int64Type();
}

const Type *FlowRunner::boolType() const
{
	return llvm::Type::getInt1Ty(cx_);
}

const Type *FlowRunner::voidType() const
{
	return llvm::Type::getVoidTy(cx_);
}

const Type *FlowRunner::arrayType() const
{
	return valueType_->getPointerTo();
}

bool FlowRunner::isArray(llvm::Value *value) const
{
	return value && value->getType() == arrayType();
}

bool FlowRunner::isArray(llvm::Type *type) const
{
	return type == arrayType();
}

bool FlowRunner::isRegExp(llvm::Value *value) const
{
	return value && value->getType() == regexpType_->getPointerTo();
}

bool FlowRunner::isIPAddress(llvm::Value *value) const
{
	return value && value->getType() == ipaddrType_->getPointerTo();
}

const Type *FlowRunner::regexpType() const
{
	return regexpType_->getPointerTo();
}

const Type *FlowRunner::ipaddrType() const
{
	return ipaddrType_->getPointerTo();
}

llvm::Value *FlowRunner::emitLoadArrayLength(llvm::Value *array)
{
	return emitCoreCall(CF::arraylen, array);
}

const Type *FlowRunner::int8Type() const
{
	return llvm::Type::getInt8Ty(cx_);
}

const Type *FlowRunner::int16Type() const
{
	return llvm::Type::getInt16Ty(cx_);
}

const Type *FlowRunner::int32Type() const
{
	return llvm::Type::getInt32Ty(cx_);
}

const Type *FlowRunner::int64Type() const
{
	return llvm::Type::getInt64Ty(cx_);
}

const Type *FlowRunner::doubleType() const
{
	return llvm::Type::getDoubleTy(cx_);
}

const Type *FlowRunner::int8PtrType() const
{
	return int8Type()->getPointerTo();
}
// }}}

// {{{ buffer API
llvm::Value *FlowRunner::emitGlobalBuffer(const std::string& value, const std::string& name)
{
	std::vector<llvm::Constant *> vec;
	vec.push_back(llvm::ConstantInt::get(int64Type(), value.size()));
	vec.push_back(llvm::ConstantArray::get(cx_, name.c_str(), false));

	llvm::Constant *initializer = llvm::ConstantStruct::get(bufferType_, vec);

	llvm::GlobalVariable *gv = new llvm::GlobalVariable(
			*module_, bufferType_, true,
			llvm::GlobalValue::InternalLinkage, initializer, name);

	return gv;
}

llvm::Value *FlowRunner::emitAllocaBuffer(llvm::Value *length, llvm::Value *data, const std::string& name)
{
	llvm::Value *nbuf = builder_.CreateAlloca(bufferType_, NULL, name);
	emitStoreBuffer(nbuf, length, data);
	return nbuf;
}

llvm::Value *FlowRunner::emitLoadBufferLength(llvm::Value *nbuf)
{
	llvm::Value *ii[2] = {
		llvm::ConstantInt::get(int32Type(), 0),
		llvm::ConstantInt::get(int32Type(), 0)
	};
	return builder_.CreateLoad(builder_.CreateInBoundsGEP(nbuf, ii, ii + 2), "load.nbuf.len");
}

llvm::Value *FlowRunner::emitLoadBufferData(llvm::Value *nbuf)
{
	llvm::Value *ii[2] = {
		llvm::ConstantInt::get(int32Type(), 0),
		llvm::ConstantInt::get(int32Type(), 1)
	};
	return builder_.CreateLoad(builder_.CreateInBoundsGEP(nbuf, ii, ii + 2), "load.nbuf.data");
}

llvm::Value *FlowRunner::emitStoreBufferLength(llvm::Value *nbuf, llvm::Value *length)
{
	llvm::Value *index[2] = {
		llvm::ConstantInt::get(int32Type(), 0),
		llvm::ConstantInt::get(int32Type(), 0)
	};
	llvm::Value *dest = builder_.CreateInBoundsGEP(nbuf, index, index + 2);
	return builder_.CreateStore(length, dest, "store.nbuf.len");
}

llvm::Value *FlowRunner::emitStoreBufferData(llvm::Value *nbuf, llvm::Value *data)
{
	llvm::Value *index[2] = {
		llvm::ConstantInt::get(int32Type(), 0),
		llvm::ConstantInt::get(int32Type(), 1)
	};
	llvm::Value *dest = builder_.CreateInBoundsGEP(nbuf, index, index + 2);
	return builder_.CreateStore(data, dest, "store.nbuf.data");
}

llvm::Value *FlowRunner::emitStoreBuffer(llvm::Value *nbuf, llvm::Value *length, llvm::Value *data)
{
	emitStoreBufferLength(nbuf, length);
	emitStoreBufferData(nbuf, data);
	return nbuf;
}

bool FlowRunner::isBufferTy(const llvm::Type *type) const
{
	return type == bufferType_;
}

bool FlowRunner::isBuffer(llvm::Value *value) const
{
	return value && isBufferTy(value->getType());
}

bool FlowRunner::isBufferPtrTy(const llvm::Type *type) const
{
	return type == bufferType_->getPointerTo();
}

bool FlowRunner::isBufferPtr(llvm::Value *value) const
{
	return value && isBufferPtrTy(value->getType());
}
// }}}

// {{{ string helper
bool FlowRunner::isCStringTy(const llvm::Type *type) const
{
	if (!type || !type->isPointerTy())
		return false;

	const llvm::PointerType *ptr = static_cast<const llvm::PointerType *>(type);

	if (!ptr->getElementType()->isIntegerTy())
		return false;

	const llvm::IntegerType *i = static_cast<const llvm::IntegerType *>(ptr->getElementType());
	if (i->getBitWidth() != 8)
		return false;

	return true;
}

bool FlowRunner::isNumber(llvm::Value *v) const
{
	return v && v->getType() == int64Type();
}

bool FlowRunner::isCString(llvm::Value *value) const
{
	return value && isCStringTy(value->getType());
}

bool FlowRunner::isString(llvm::Value *v1) const
{
	return isCString(v1) || isBufferPtr(v1);
}

bool FlowRunner::isStringPair(llvm::Value *v1, llvm::Value *v2) const
{
	return (isCString(v1) || isBufferPtr(v1))
		&& (isCString(v2) || isBufferPtr(v2));
}

bool FlowRunner::isFunctionPtr(llvm::Value *value) const
{
	if (!value->getType()->isPointerTy())
		return false;

	const llvm::PointerType *ptr = static_cast<const llvm::PointerType *>(value->getType());
	const llvm::Type *elementType = ptr->getElementType();

	if (!elementType->isFunctionTy())
		return false;

	// TODO: check prototype: i1 (i8* cx_udata)

	return true;
}
// }}}

// {{{ symbols
void FlowRunner::visit(Variable& var)
{
	FNTRACE();
	TRACE("variable '%s'", var.name().c_str());

	if (!var.parentScope())
	{
		int id = findNative(var.name());
		if (id == -1)
		{
			reportError("undefined global variable '%s'", var.name().c_str());
			return;
		}
		emitNativeCall(id, NULL);
		return;
	}

	bool isLocal = var.parentScope()->outerTable();
	if (isLocal) {
		// local variables: put into the functions entry block with an alloca inbufuction
		TRACE("local variable '%s'", var.name().c_str());
		llvm::Value *initialValue = codegen(var.value());
		if (!initialValue)
			return;

		llvm::Function *fn = builder_.GetInsertBlock()->getParent();

		llvm::IRBuilder<> ebb(&fn->getEntryBlock(), fn->getEntryBlock().begin());

		value_ = ebb.CreateAlloca(initialValue->getType(), 0, (var.name() + ".ptr").c_str());
		builder_.CreateStore(initialValue, value_);

		scope_.insert(&var, value_);
	} else {
		// global variable
		TRACE("global variable '%s'", var.name().c_str());

		llvm::BasicBlock *lastBB = builder_.GetInsertBlock();
		builder_.SetInsertPoint(initializerBB_);
		value_ = codegen(var.value());
		initializerBB_ = builder_.GetInsertBlock(); // update BB in case it's been updated

		scope_.insertGlobal(&var, value_);

		// restore callers BB
		if (lastBB)
			builder_.SetInsertPoint(lastBB);
		else
			builder_.ClearInsertionPoint();
	}
}

// convert a Flow-type id into an LLVM type object
const llvm::Type *FlowRunner::makeType(FlowToken t) const
{
	switch (t) {
		case FlowToken::Void:
			return llvm::Type::getVoidTy(cx_);
		case FlowToken::Boolean:
			return llvm::Type::getInt1Ty(cx_);
		case FlowToken::Int:
			return llvm::Type::getInt32Ty(cx_);
		case FlowToken::Long:
			return llvm::Type::getInt32Ty(cx_);
		case FlowToken::LongLong:
			return llvm::Type::getInt64Ty(cx_);
		case FlowToken::String:
			return llvm::Type::getInt8PtrTy(cx_);// XXX is this right?
		case FlowToken::Float:
			return llvm::Type::getFloatTy(cx_);
		case FlowToken::Double:
			return llvm::Type::getDoubleTy(cx_);
		default:
			fprintf(stderr, "invalid type: %d\n", (int)t);
			// TODO remaining types
			return llvm::Type::getVoidTy(cx_);
	}
}

extern "C" int flow_endsWidth(const char *left, const char *right)
{
	size_t ll = strlen(left);
	size_t lr = strlen(right);

	if (lr > ll)
		return 1;

	if (strcasecmp(left + (ll - lr), right) != 0)
		return 1;

	return 0;
}

/**
 * \brief calculates the length of the given array.
 *
 * \return the calculated array length.
 */
extern "C" uint32_t flow_arraylen(FlowValue *array)
{
	uint32_t result = 0;
	while (!array->isVoid())
	{
		++array;
		++result;
	}

	return result;
}

/**
 * \brief cats two value arrays together.
 *
 * \param result the array the result is to be stored to
 * \param left the left input array stored first
 * \param right the right input array stored after the left array into the result
 */
extern "C" void flow_arrayadd(FlowValue *result, FlowValue *left, FlowValue *right)
{
	while (!left->isVoid())
		(result++)->set(*left++);

	while (!right->isVoid())
		(result++)->set(*right++);

	result->clear();
}

/**
 * \brief compares two arrays.
 * \retval 0 equal
 * \retval 1 not equal
 */
extern "C" int32_t flow_arraycmp(const FlowValue *left, const FlowValue *right)
{
	while (!left->isVoid() && !right->isVoid())
	{
		// compare types
		if (left->type() != right->type())
			return 1;

		// TODO: compare actual values
		bool test = false;
		switch (left->type())
		{
			case FlowValue::NUMBER:
				test = left->toNumber() == right->toNumber();
				break;
			case FlowValue::STRING:
				test = strcasecmp(left->toString(), right->toString()) == 0;
				break;
			case FlowValue::BOOLEAN:
				test = left->toBool() == right->toBool();
				break;
			default:
				break;
		}
		if (!test)
			return 1;

		++left;
		++right;
	}

	if (left->isVoid() && right->isVoid())
		return 0;

	return 1;
}

/**
 * \brief tests whether given \p text matches regular expression \p pattern.
 *
 * \retval 0 not matched.
 * \retval 1 matched.
 */
extern "C" int flow_regexmatch(size_t textLength, const char *text, size_t patternLength, const char *pattern)
{
	RegExp re(std::string(pattern, patternLength));
	return re.match(text, textLength);
}

extern "C" int flow_regexmatch2(size_t textLength, const char *text, const RegExp *re)
{
	return re->match(text, textLength);
}

extern "C" int flow_NumberInArray(uint64_t number, const FlowValue *array)
{
	for (; !array->isVoid(); ++array)
	{
		switch (array->type())
		{
			case FlowValue::NUMBER:
				if (number == array->toNumber())
					return true;
				break;
			default:
				break;
		}
	}

	return false;
}

/** compares an IPAddress object with a string representation of an IP address.
 * \return zero on equality, non-zero if not.
 */
extern "C" int flow_ipstrcmp(const IPAddress *ipaddr, const char *string)
{
	return strcmp(ipaddr->str().c_str(), string);
}

/** compares two IPAddress objects.
 * \retval 0 equal
 * \retval 1 unequal
 */
extern "C" int flow_ipcmp(const IPAddress *ip1, const IPAddress *ip2)
{
	return *ip1 == *ip2 ? 0 : 1;
}

extern "C" int flow_StringInArray(size_t textLength, const char *text, const FlowValue *array)
{
	for (; !array->isVoid(); ++array)
	{
		switch (array->type())
		{
			case FlowValue::STRING:
				if (textLength == strlen(array->toString()))
					if (strncasecmp(text, array->toString(), textLength) == 0)
						return true;
				break;
			case FlowValue::BUFFER:
				if (array->toNumber() == textLength)
				{
					const char *t = array->toString();
					const char *u = text;
					size_t i = textLength;

					while (i)
					{
						if (std::tolower(*t) == std::tolower(*u))
							return true;

						++t;
						++u;
						--i;
					}
				}
				break;
			default:
				break;
		}
	}

	return false;
}

void FlowRunner::emitCoreFunctions()
{
	emitCoreFunction(CF::strlen, "strlen", int64Type(), stringType(), false);
	emitCoreFunction(CF::strcat, "strcat", stringType(), stringType(), stringType(), false);
	emitCoreFunction(CF::strcpy, "strcpy", stringType(), stringType(), stringType(), false);
	emitCoreFunction(CF::memcpy, "memcpy", stringType(), stringType(), stringType(), int64Type(), false);

	emitCoreFunction(CF::strcasecmp, "strcasecmp", int32Type(), stringType(), stringType(), false);
	emitCoreFunction(CF::strncasecmp, "strncasecmp", int32Type(), stringType(), stringType(), int64Type(), false);
	emitCoreFunction(CF::strcasestr, "strcasestr", stringType(), stringType(), stringType(), false);

	emitCoreFunction(CF::strcmp, "strcmp", int32Type(), stringType(), stringType(), false);
	emitCoreFunction(CF::strncmp, "strncmp", int32Type(), stringType(), stringType(), false);

	emitCoreFunction(CF::endsWith, "flow_endsWidth", int32Type(), stringType(), stringType(), false);

	emitCoreFunction(CF::arraylen, "flow_arraylen", int32Type(), arrayType(), false);
	emitCoreFunction(CF::arrayadd, "flow_arrayadd", voidType(), arrayType(), arrayType(), arrayType(), false);
	emitCoreFunction(CF::arraycmp, "flow_arraycmp", int32Type(), arrayType(), arrayType(), false);

	emitCoreFunction(CF::regexmatch, "flow_regexmatch", int32Type(), int64Type(), stringType(), int64Type(), stringType(), false);
	emitCoreFunction(CF::regexmatch2, "flow_regexmatch2", int32Type(), int64Type(), stringType(), regexpType_->getPointerTo(), false);

	emitCoreFunction(CF::NumberInArray, "flow_NumberInArray", int32Type(), int64Type(), arrayType(), false);
	emitCoreFunction(CF::StringInArray, "flow_StringInArray", int32Type(), int64Type(), stringType(), arrayType(), false);

	emitCoreFunction(CF::ipstrcmp, "flow_ipstrcmp", int32Type(), ipaddrType(), stringType(), false);
	emitCoreFunction(CF::ipcmp, "flow_ipcmp", int32Type(), ipaddrType(), ipaddrType(), false);
	emitCoreFunction(CF::pow, "llvm.pow.f64", doubleType(), doubleType(), doubleType(), false);
}

void FlowRunner::emitCoreFunction(CF id, const std::string& name, const Type *rt, const Type *p1, bool isVaArg)
{
	const Type *p[1] = { p1 };
	emitCoreFunction(id, name, rt, p, p + sizeof(p) / sizeof(*p), isVaArg);
}

void FlowRunner::emitCoreFunction(CF id, const std::string& name, const Type *rt, const Type *p1, const Type *p2, bool isVaArg)
{
	const Type *p[2] = { p1, p2 };
	emitCoreFunction(id, name, rt, p, p + sizeof(p) / sizeof(*p), isVaArg);
}

void FlowRunner::emitCoreFunction(CF id, const std::string& name, const Type *rt, const Type *p1, const Type *p2, const Type *p3, bool isVaArg)
{
	const Type *p[3] = { p1, p2, p3 };
	emitCoreFunction(id, name, rt, p, p + sizeof(p) / sizeof(*p), isVaArg);
}

void FlowRunner::emitCoreFunction(CF id, const std::string& name, const Type *rt, const Type *p1, const Type *p2, const Type *p3, const Type *p4, bool isVaArg)
{
	const Type *p[4] = { p1, p2, p3, p4 };
	emitCoreFunction(id, name, rt, p, p + sizeof(p) / sizeof(*p), isVaArg);
}

template<typename T>
void FlowRunner::emitCoreFunction(CF id, const std::string& name, const Type *rt, T pbegin, T pend, bool isVaArg)
{
	std::vector<const Type *> params;
	for (; pbegin != pend; ++pbegin)
		params.push_back(*pbegin);

	coreFunctions_[static_cast<int>(id)] = llvm::Function::Create(
		llvm::FunctionType::get(rt, params, isVaArg),
		llvm::Function::ExternalLinkage,
		name,
		module_
	);
}

void FlowRunner::emitNativeFunctionSignature()
{
	std::vector<const llvm::Type *> argTypes;
	argTypes.push_back(int64Type()); // self ptr
	argTypes.push_back(int32Type()); // function id
	argTypes.push_back(int8PtrType()); // context userdata
	argTypes.push_back(int32Type()); // argc
	argTypes.push_back(valueType_->getPointerTo()); // FlowValue *argv

	const llvm::FunctionType *ft = llvm::FunctionType::get(
		voidType(), // return type
		argTypes,   // arg types
		false       // isVaArg
	);

	coreFunctions_[0] = llvm::Function::Create(
		ft,
		llvm::Function::ExternalLinkage,
		"flow_backend_callback",
		module_
	);
}

void FlowRunner::emitNativeValue(int index, llvm::Value *lhs, llvm::Value *rhs)
{
	int typeCode;
	llvm::Value *valueIndices[2];

	valueIndices[0] = llvm::ConstantInt::get(llvm::Type::getInt32Ty(cx_), index);

	if (!rhs)
	{
		typeCode = FlowValue::VOID;
	}
	else if (isBool(rhs))
	{
		typeCode = FlowValue::BOOLEAN;
		rhs = builder_.CreateIntCast(rhs, numberType(), false, "bool2int");
		valueIndices[1] = llvm::ConstantInt::get(llvm::Type::getInt32Ty(cx_), FlowValue::NumberOffset);
		builder_.CreateStore(rhs, builder_.CreateInBoundsGEP(lhs, valueIndices, valueIndices + 2), "store.arg.value");
	}
	else if (rhs->getType()->isIntegerTy())
	{
		typeCode = FlowValue::NUMBER;
		valueIndices[1] = llvm::ConstantInt::get(llvm::Type::getInt32Ty(cx_), FlowValue::NumberOffset);
		builder_.CreateStore(rhs, builder_.CreateInBoundsGEP(lhs, valueIndices, valueIndices + 2), "store.arg.value");
	}
	else if (isArray(rhs)) // some expression list
	{
		typeCode = FlowValue::ARRAY;

		valueIndices[1] = llvm::ConstantInt::get(llvm::Type::getInt32Ty(cx_), FlowValue::ArrayOffset);
		rhs = builder_.CreateBitCast(rhs, int8PtrType());
		builder_.CreateStore(rhs, builder_.CreateInBoundsGEP(lhs, valueIndices, valueIndices + 2), "stor.ary");
	}
	else if (isRegExp(rhs))
	{
		typeCode = FlowValue::REGEXP;

		valueIndices[1] = llvm::ConstantInt::get(llvm::Type::getInt32Ty(cx_), FlowValue::RegExpOffset);
		rhs = builder_.CreateBitCast(rhs, int8PtrType());
		builder_.CreateStore(rhs, builder_.CreateInBoundsGEP(lhs, valueIndices, valueIndices + 2), "stor.regexp");
	}
	else if (isIPAddress(rhs))
	{
		typeCode = FlowValue::IP;

		valueIndices[1] = llvm::ConstantInt::get(llvm::Type::getInt32Ty(cx_), FlowValue::IPAddrOffset);
		rhs = builder_.CreateBitCast(rhs, int8PtrType());
		builder_.CreateStore(rhs, builder_.CreateInBoundsGEP(lhs, valueIndices, valueIndices + 2), "stor.ip");
	}
	else if (isFunctionPtr(rhs))
	{
		typeCode = FlowValue::FUNCTION;
		valueIndices[1] = llvm::ConstantInt::get(llvm::Type::getInt32Ty(cx_), FlowValue::FunctionOffset);
		rhs = builder_.CreateBitCast(rhs, int8PtrType());
		builder_.CreateStore(rhs, builder_.CreateInBoundsGEP(lhs, valueIndices, valueIndices + 2), "stor.fnref");
	}
	else if (isCString(rhs))
	{
		typeCode = FlowValue::STRING;

		valueIndices[1] = llvm::ConstantInt::get(llvm::Type::getInt32Ty(cx_), FlowValue::BufferOffset);
		builder_.CreateStore(rhs, builder_.CreateInBoundsGEP(lhs, valueIndices, valueIndices + 2), "stor.str");
	}
	else if (isBufferPtr(rhs))
	{
		typeCode = FlowValue::BUFFER;

		llvm::Value *len = emitLoadBufferLength(rhs);
		llvm::Value *buf = emitLoadBufferData(rhs);

		valueIndices[1] = llvm::ConstantInt::get(llvm::Type::getInt32Ty(cx_), FlowValue::NumberOffset);
		builder_.CreateStore(len, builder_.CreateInBoundsGEP(lhs, valueIndices, valueIndices + 2), "stor.len");

		valueIndices[1] = llvm::ConstantInt::get(llvm::Type::getInt32Ty(cx_), FlowValue::BufferOffset);
		builder_.CreateStore(buf, builder_.CreateInBoundsGEP(lhs, valueIndices, valueIndices + 2), "stor.len");
	}
	else
	{
		printf("emit native value of unknown type? (%d)\n", rhs->getType()->isFunctionTy());
		typeCode = FlowValue::VOID;
		rhs->dump();
		printf("type:\n");
		rhs->getType()->dump();
		printf("lhs:\n");
		lhs->dump();
	}

	// store values type code
	llvm::Value *typeIndices[2] = {
		llvm::ConstantInt::get(llvm::Type::getInt32Ty(cx_), index),
		llvm::ConstantInt::get(llvm::Type::getInt32Ty(cx_), 0)
	};
	builder_.CreateStore(
		llvm::ConstantInt::get(llvm::Type::getInt32Ty(cx_), typeCode),
		builder_.CreateInBoundsGEP(lhs, typeIndices, typeIndices + 2, "arg.type"),
		"store.arg.type");
}

/** emits the native-callback function call to call back to the host process to 
  * actually invoke the function.
  */
void FlowRunner::emitNativeCall(int id, ListExpr *argList)
{
	FNTRACE();

	// prepare handler parameters
	llvm::Value *callArgs[5];

	callArgs[0] = llvm::ConstantInt::get(llvm::Type::getInt64Ty(cx_), reinterpret_cast<uint64_t>(backend_), false); // self
	callArgs[1] = llvm::ConstantInt::get(llvm::Type::getInt32Ty(cx_), id, false); // native callback id
	callArgs[2] = scope_.lookup(NULL); // context userdata

	// argc:
	int argc = argList ? argList->length() : 0;
	callArgs[3] = llvm::ConstantInt::get(llvm::Type::getInt32Ty(cx_), argc);

	// argv:
	callArgs[4] = builder_.CreateAlloca(valueType_,
		llvm::ConstantInt::get(llvm::Type::getInt32Ty(cx_), argc + 1), "args.ptr");

	emitNativeValue(0, callArgs[4], NULL); // initialize return value

	int index = 1;
	if (argc)
		for (auto i = argList->begin(), e = argList->end(); i != e; ++i)
			emitNativeValue(index++, callArgs[4], codegen(*i));

	// emit call
	value_ = builder_.CreateCall(coreFunctions_[0], callArgs, callArgs + sizeof(callArgs) / sizeof(*callArgs));

	// handle return value
	FlowBackend::Callback *native = backend_->at(id);

	switch (native->type)
	{
		case FlowBackend::Callback::VARIABLE:
		case FlowBackend::Callback::FUNCTION:
		{
			if (native->returnType == FlowValue::BUFFER)
			{
				// retrieve buffer length
				llvm::Value *valueIndices[2] = {
					llvm::ConstantInt::get(llvm::Type::getInt32Ty(cx_), FlowValue::TypeOffset),
					llvm::ConstantInt::get(llvm::Type::getInt32Ty(cx_), FlowValue::NumberOffset)
				};
				value_ = builder_.CreateInBoundsGEP(callArgs[4], valueIndices, valueIndices + 2, "retval.buflen.tmp");
				llvm::Value *length = builder_.CreateLoad(value_, "retval.buflen.load");

				// retrieve ref to buffer data
				valueIndices[1] = llvm::ConstantInt::get(llvm::Type::getInt32Ty(cx_), FlowValue::BufferOffset);
				value_ = builder_.CreateInBoundsGEP(callArgs[4], valueIndices, valueIndices + 2, "retval.buf.tmp");
				llvm::Value *data = builder_.CreateLoad(value_, "retval.buf.load");

				value_ = emitAllocaBuffer(length, data, "retval");
			}
			else // no buffer
			{
				int valueIndex;
				switch (native->returnType) {
					case FlowValue::BOOLEAN: valueIndex = FlowValue::NumberOffset; break;
					case FlowValue::NUMBER: valueIndex = FlowValue::NumberOffset; break;
					case FlowValue::STRING: valueIndex = FlowValue::BufferOffset; break;
					default: valueIndex = 0; break;
				}

				llvm::Value *valueIndices[2] = {
					llvm::ConstantInt::get(llvm::Type::getInt32Ty(cx_), 0),
					llvm::ConstantInt::get(llvm::Type::getInt32Ty(cx_), valueIndex)
				};
				value_ = builder_.CreateInBoundsGEP(callArgs[4], valueIndices, valueIndices + 2, "retval.value.tmp");
				value_ = builder_.CreateLoad(value_, "retval.value.load");
			}

			break;
		}
		case FlowBackend::Callback::HANDLER:
		{
			llvm::Value *valueIndices[2] = {
				llvm::ConstantInt::get(llvm::Type::getInt32Ty(cx_), 0),
				llvm::ConstantInt::get(llvm::Type::getInt32Ty(cx_), FlowValue::NumberOffset)
			};
			value_ = builder_.CreateInBoundsGEP(callArgs[4], valueIndices, valueIndices + 2, "retval.value.tmp");
			value_ = builder_.CreateLoad(value_, "retval.value.load");

			// compare return value for not being false (zero)
			value_ = builder_.CreateICmpNE(value_, llvm::ConstantInt::get(numberType(), 0));

			// restore outer BB insert-point & leave scope
			llvm::Function *caller = builder_.GetInsertBlock()->getParent();
			llvm::BasicBlock *doneBlock = llvm::BasicBlock::Create(cx_, "handler.done", caller);
			llvm::BasicBlock *contBlock = llvm::BasicBlock::Create(cx_, "handler.cont");
			builder_.CreateCondBr(value_, doneBlock, contBlock);

			// emit handler.done block
			builder_.SetInsertPoint(doneBlock);
			builder_.CreateRet(llvm::ConstantInt::get(llvm::Type::getInt1Ty(cx_), 1));
			doneBlock = builder_.GetInsertBlock();

			// emit handler.cont block
			caller->getBasicBlockList().push_back(contBlock);
			builder_.SetInsertPoint(contBlock);

			break;
		}
		default:
			reportError("Unknown callback type (%d) encountered.", native->type);
			break;
	}
}

void FlowRunner::visit(Function& function)
{
	FNTRACE();

	if (findNative(function.name()) != -1)
	{
		TRACE("native callback decl '%s'.\n", function.name().c_str());
		return;
	}

	if (function.body() == NULL)
	{
		reportError("Cannot use unknown function '%s'.", function.name().c_str());
		return;
	}

	// conbufuct function proto-type
	std::vector<const llvm::Type *> argTypes;
	if (function.isHandler())
		argTypes.push_back(int8PtrType());

	for (auto i = function.argTypes()->begin(), e = function.argTypes()->end(); i != e; ++i)
		argTypes.push_back(makeType(*i));

	llvm::FunctionType *ft = llvm::FunctionType::get(
		makeType(function.returnType()), argTypes, function.isVarArg());

	llvm::Function *fn = llvm::Function::Create(
		ft, llvm::Function::ExternalLinkage, function.name(), module_);

	functions_.push_back(fn);

	if (!function.body()) { // external function
		value_ = fn;
		scope_.insertGlobal(&function, value_);
		return;
	}

	scope_.enter();

	{
		size_t i = 0;
		for (llvm::Function::arg_iterator ai = fn->arg_begin(); i != argTypes.size() + 1; ++ai, ++i) {
			if (i == 0) {
				ai->setName("cx_udata");
				scope_.insert(NULL, ai);
			} else {
				// TODO
				//ai->setName("arg");
				//scope_.insert(function.argTypes()[i - 1], ai);
			}
		}
	}

	// create entry-BasicBlock for this function and enter inner scope
	llvm::BasicBlock *lastBB = builder_.GetInsertBlock();
	llvm::BasicBlock *bb = llvm::BasicBlock::Create(cx_, "entry", fn);
	builder_.SetInsertPoint(bb);

	// generate code: local-scope variables
	for (auto i = function.scope()->begin(), e = function.scope()->end(); i != e; ++i)
		codegen(*i);

	// generate code: function body
	codegen(function.body());

	// generate code: catch-all return
	if (function.isHandler())
		builder_.CreateRet(llvm::ConstantInt::get(llvm::Type::getInt1Ty(cx_), 0));
	else
		builder_.CreateRetVoid();

	//dump("verify function");
	llvm::verifyFunction(*fn);

	// perform function-level optimizations
	if (functionPassMgr_)
		functionPassMgr_->run(*fn);

	// restore outer BB insert-point & leave scope
	scope_.leave();
	if (lastBB)
		builder_.SetInsertPoint(lastBB);
	else
		builder_.ClearInsertionPoint();

	value_ = fn;
	scope_.insertGlobal(&function, value_);
}

void FlowRunner::visit(Unit& unit)
{
	FNTRACE();

	for (size_t i = 0, e = unit.importCount(); i != e; ++i)
		backend_->import(unit.getImportName(i), unit.getImportPath(i));

	emitInitializerHead();

	// emit all handlers (and their dependancies)
	for (auto i = unit.members()->begin(), e = unit.members()->end(); i != e; ++i)
		if ((*i)->isFunction())
			codegen(*i);

	emitInitializerTail();

	value_ = NULL;
}

void FlowRunner::emitInitializerHead()
{
	std::vector<const llvm::Type *> argTypes;
	llvm::FunctionType *ft = llvm::FunctionType::get(llvm::Type::getVoidTy(cx_), argTypes, false);
	initializerFn_ = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "__flow_initialize", module_);
	initializerBB_ = llvm::BasicBlock::Create(cx_, "entry", initializerFn_);
}

void FlowRunner::emitInitializerTail()
{
	llvm::BasicBlock *lastBB = builder_.GetInsertBlock();
	builder_.SetInsertPoint(initializerBB_);

	builder_.CreateRetVoid();

	if (lastBB)
		builder_.SetInsertPoint(lastBB);
	else
		builder_.ClearInsertionPoint();

	llvm::verifyFunction(*initializerFn_);

	if (functionPassMgr_)
		functionPassMgr_->run(*initializerFn_);
}
// }}}

// {{{ codegen support
llvm::Value *FlowRunner::toBool(llvm::Value *value)
{
	if (isBoolTy(value->getType()))
		return value;

	if (value->getType()->isIntegerTy())
		return builder_.CreateICmpNE(value, llvm::ConstantInt::get(value->getType(), 0), "int2bool");

	if (isString(value))
	{
		llvm::Value *slen = emitLoadStringLength(value);
		return builder_.CreateICmpNE(
			slen,
			llvm::ConstantInt::get(slen->getType(), 0),
			"str2bool");
	}

	if (value->getType()->isFloatTy()
			|| value->getType()->isDoubleTy()
			|| value->getType()->isX86_FP80Ty())
	{
		return builder_.CreateFPToSI(
			builder_.CreateFCmpONE(value, llvm::ConstantFP::get(value->getType(), 0), "fp2bool"),
			numberType()
		);
	}

	// TODO other type casts
	reportError("toBool: cast of unknown type ignored");
	value->dump();

	return value;
}

llvm::Value *FlowRunner::emitGetUMin(llvm::Value *len1, llvm::Value *len2)
{
	return NULL; // TODO
}

llvm::Value *FlowRunner::emitToLower(llvm::Value *value)
{
#if 1 == 0 // {{{ C equivalent
	return value >= 'A' && value <= 'Z'
		? value + 32
		: value;
#endif // }}}

	// IR:
	//   %off = add %ch, -65
	//   %lwr = add %ch, 32
	//   %cnd = icmp ult %off, 26
	//   %res = select %cnd, %lwr, %ch

	llvm::Value *off = builder_.CreateSub(value, llvm::ConstantInt::get(value->getType(), 65));
	llvm::Value *lwr = builder_.CreateAdd(value, llvm::ConstantInt::get(value->getType(), 32));
	llvm::Value *cnd = builder_.CreateICmpULT(off, llvm::ConstantInt::get(value->getType(), 26));
	llvm::Value *res = builder_.CreateSelect(cnd, lwr, value);

	return res;
}

/** emits a (case insensitive) string compare.
 * \param len1 length (i64) of buf1
 * \param buf1 an i8* to a string buffer
 * \param len2 length (i64) of buf2
 * \param buf2 an i8* to a string buffer
 *
 * \retval <0 buf1 is less then buf2
 * \retval =0 buf1 equals buf2
 * \retval >0 buf1 is greater then buf2
 */
llvm::Value *FlowRunner::emitCmpString(
	llvm::Value *len1, llvm::Value *buf1,
	llvm::Value *len2, llvm::Value *buf2)
{
#if 1 == 0 // {{{ C equivalent
	int d;
	for (;;)
	{
		d = len1 - len2;

		if ((len1 & len2) == 0) // equals !(len1 && len2)
			break;

		d = tolower(*buf1) - tolower(*buf2);
		if (d != 0)
			break;

		len1--; len2--; buf1++; buf2++;
	}
	return d;
#endif // }}}

	// {{{ IR
	// goto loop.cmp1
	//
	// loop.cmp1:
	//   d1 = sub len1, len2
	//   cond1 = cmp eq (len1 & len2), 0
	//   br cond1, loop.end, loop.cmp1
	//
	// loop.cmp2:
	//   d2 = sub *buf1, *buf2
	//   cond2 = cmp ne d2, 0
	//   br cond2, loop.end, loop.tail
	//
	// loop.tail:
	//   buf1++
	//   buf2++
	//   len1--
	//   len2--
	//
	// loop.end:
	//   result = phi [d1, loop.cmp1], [d2, loop.cmp2]
	// }}}

	llvm::Function *caller = builder_.GetInsertBlock()->getParent();
	llvm::BasicBlock *cmp1BB = llvm::BasicBlock::Create(cx_, "loop.cmp1");
	llvm::BasicBlock *cmp2BB = llvm::BasicBlock::Create(cx_, "loop.cmp2");
	llvm::BasicBlock *tailBB = llvm::BasicBlock::Create(cx_, "loop.tail");
	llvm::BasicBlock *endBB = llvm::BasicBlock::Create(cx_, "loop.end");

	// create temporaries
	llvm::IRBuilder<> ebb(&caller->getEntryBlock(), caller->getEntryBlock().begin());

	llvm::Value *len1ptr = ebb.CreateAlloca(int64Type(), NULL, "len1.ptr");
	llvm::Value *len2ptr = ebb.CreateAlloca(int64Type(), NULL, "len2.ptr");
	llvm::Value *buf1ptr = ebb.CreateAlloca(int8PtrType(), NULL, "buf1.ptr");
	llvm::Value *buf2ptr = ebb.CreateAlloca(int8PtrType(), NULL, "buf2.ptr");
	llvm::Value *d = ebb.CreateAlloca(int64Type(), NULL, "d");

	builder_.CreateStore(len1, len1ptr);
	builder_.CreateStore(len2, len2ptr);
	builder_.CreateStore(buf1, buf1ptr);
	builder_.CreateStore(buf2, buf2ptr);
	builder_.CreateBr(cmp1BB);

	// cmp1BB[d1]:    if ((len1 & len2) == 0) goto end
	caller->getBasicBlockList().push_back(cmp1BB);
	builder_.SetInsertPoint(cmp1BB);
	len1 = builder_.CreateLoad(len1ptr);
	len2 = builder_.CreateLoad(len2ptr);
	llvm::Value *d1 = builder_.CreateSub(len1, len2, "d1");
	builder_.CreateStore(d1, d);
	llvm::Value *cmp = builder_.CreateAnd(len1, len2, "len1&len2");
	cmp = builder_.CreateICmpEQ(cmp, llvm::ConstantInt::get(int64Type(), 0));
	builder_.CreateCondBr(cmp, endBB, cmp2BB);
	cmp1BB = builder_.GetInsertBlock();

	// cmp2BB[d2]:    if ((tolower(*buf1) - tolower(*buf2)) != 0) goto end
	caller->getBasicBlockList().push_back(cmp2BB);
	builder_.SetInsertPoint(cmp2BB);
	llvm::Value *v1 = emitToLower(builder_.CreateLoad(builder_.CreateLoad(buf1ptr), "v1"));
	llvm::Value *v2 = emitToLower(builder_.CreateLoad(builder_.CreateLoad(buf2ptr), "v2"));
	llvm::Value *d2 = builder_.CreateIntCast(builder_.CreateSub(v1, v2, "subv"), int64Type(), true, "d2");
	builder_.CreateStore(d2, d);
	llvm::Value *cc = builder_.CreateICmpNE(d2, llvm::ConstantInt::get(int64Type(), 0), "cc");
	builder_.CreateCondBr(cc, endBB, tailBB);
	cmp2BB = builder_.GetInsertBlock();

	// tailBB:        --len1; --len2; ++buf1; ++buf2;
	caller->getBasicBlockList().push_back(tailBB);
	builder_.SetInsertPoint(tailBB);
	len1 = builder_.CreateSub(builder_.CreateLoad(len1ptr), llvm::ConstantInt::get(int64Type(), 1), "len1dec");
	len2 = builder_.CreateSub(builder_.CreateLoad(len2ptr), llvm::ConstantInt::get(int64Type(), 1), "len2dec");
	builder_.CreateStore(len1, len1ptr);
	builder_.CreateStore(len2, len2ptr);
	buf1 = builder_.CreateInBoundsGEP(builder_.CreateLoad(buf1ptr), llvm::ConstantInt::get(int64Type(), 1), "buf1inc");
	buf2 = builder_.CreateInBoundsGEP(builder_.CreateLoad(buf2ptr), llvm::ConstantInt::get(int64Type(), 1), "buf2inc");
	builder_.CreateStore(buf1, buf1ptr);
	builder_.CreateStore(buf2, buf2ptr);
	builder_.CreateBr(cmp1BB);
	tailBB = builder_.GetInsertBlock();

	// endBB: phi (d1, d2)
	caller->getBasicBlockList().push_back(endBB);
	builder_.SetInsertPoint(endBB);
	return builder_.CreateLoad(d);
}

llvm::Value *FlowRunner::emitCmpString(Operator op, llvm::Value *left, llvm::Value *right)
{
	llvm::Value *len1;
	llvm::Value *buf1;
	llvm::Value *len2;
	llvm::Value *buf2;

	if (isBufferPtr(left)) {
		len1 = emitLoadBufferLength(left);
		buf1 = emitLoadBufferData(left);
	} else {
		len1 = emitCoreCall(CF::strlen, left);
		buf1 = left;
	}

	if (isBufferPtr(right)) {
		len2 = emitLoadBufferLength(right);
		buf2 = emitLoadBufferData(right);
	} else {
		len2 = emitCoreCall(CF::strlen, right);
		buf2 = right;
	}

	llvm::Value *rv = op == Operator::RegexMatch
		? emitCoreCall(CF::regexmatch, len1, buf1, len2, buf2)
		: emitCmpString(len1, buf1, len2, buf2);

	switch (op)
	{
		case Operator::RegexMatch:
			return builder_.CreateICmpNE(rv, llvm::ConstantInt::get(int32Type(), 0));
		case Operator::Equal:
			return builder_.CreateICmpEQ(rv, llvm::ConstantInt::get(int64Type(), 0));
		case Operator::UnEqual:
			return builder_.CreateICmpNE(rv, llvm::ConstantInt::get(int64Type(), 0));
		case Operator::Less:
			return builder_.CreateICmpSLT(rv, llvm::ConstantInt::get(int64Type(), 0));
		case Operator::Greater:
			return builder_.CreateICmpSGT(rv, llvm::ConstantInt::get(int64Type(), 0));
		case Operator::LessOrEqual:
			return builder_.CreateICmpSLE(rv, llvm::ConstantInt::get(int64Type(), 0));
		case Operator::GreaterOrEqual:
			return builder_.CreateICmpSGE(rv, llvm::ConstantInt::get(int64Type(), 0));
		default:
			return NULL;
	}
}

/** \brief kinda like strcasestr(v1, v2). asdfljk
  * \param haystack pointer to a C-string or an buffer
  */
llvm::Value *FlowRunner::emitStrCaseStr(llvm::Value *haystack, llvm::Value *needle)
{
	return emitCoreCall(CF::strcasestr, haystack, needle);
}

llvm::Value *FlowRunner::emitIsSubString(llvm::Value *haystack, llvm::Value *needle)
{
	// TODO (buffer, buffer)
	// TODO (buffer, string)
	// TODO (string, buffer)

	value_ = builder_.CreatePtrToInt(
			emitStrCaseStr(haystack, needle),
			llvm::Type::getInt64Ty(cx_));

	value_ = builder_.CreateICmpNE(
			value_,
			llvm::ConstantInt::get(llvm::Type::getInt64Ty(cx_), 0),
			"issubstrof");

	return value_;
}

/** \brief emits code to glue two strings together.
  * \return the new (C-)string containing v1 followed by v2.
  */
llvm::Value *FlowRunner::emitStringCat(llvm::Value *v1, llvm::Value *v2)
{
	llvm::Value *ll = emitLoadStringLength(v1);
	llvm::Value *rn = emitLoadStringLength(v2);

	v1 = emitLoadStringBuffer(v1);
	v2 = emitLoadStringBuffer(v2);

	// len = ll + rn + 1;
	llvm::Value *len = builder_.CreateAdd(ll, rn, "len.sum");
	len = builder_.CreateAdd(len, llvm::ConstantInt::get(int64Type(), 1), "len.zsum");

	// compose buffer
	llvm::Value *result = builder_.CreateAlloca(int8Type(), builder_.CreateIntCast(len, int32Type(), false), "strcat.ptr");
	llvm::Value *midptr = builder_.CreateInBoundsGEP(result, ll, "mid.ptr");

	emitCoreCall(CF::memcpy, result, v1, ll);
	emitCoreCall(CF::memcpy, midptr, v2, rn);

	// store EOS
	llvm::Value *eos = builder_.CreateInBoundsGEP(midptr, rn, "eos.ptr");
	builder_.CreateStore(llvm::ConstantInt::get(int8Type(), 0), eos, "mk.eos");

	return result;
}

/** \brief retrieves the length value of a string (C-string or nbuf)
  * \return i64 value, containing the length
  */
llvm::Value *FlowRunner::emitLoadStringLength(llvm::Value *value)
{
	if (isBufferPtr(value))
		return emitLoadBufferLength(value);
	else if (isCString(value))
		return emitCoreCall(CF::strlen, value);
	else
		return NULL;
}

/**
  * \brief retrieves a reference to the first char of the string (C-string or nbuf)
  * \return i8* to the buffer containing the string.
  */
llvm::Value *FlowRunner::emitLoadStringBuffer(llvm::Value *value)
{
	if (isBufferPtr(value))
		return emitLoadBufferData(value);
	else if (isCString(value))
		return value;
	else
		return NULL;
}

llvm::Value *FlowRunner::emitPrefixMatch(llvm::Value *left, llvm::Value *right)
{
#if 1 == 0 // {{{ C equivalent
	// left =^ right
	size_t l1 = strlen(left);
	size_t l2 = strlen(right);

	if (l2 > l1)
		return false;

	if (strncasecmp(left, right, l1) != 0)
		return false;

	return true;
#endif // }}}

	// {{{ IR
	//   %l1 = strlen %left
	//   %l2 = strlen %right
	//   %result = alloca i1
	//   store 0, %result
	//
	//   %tmp = cmp gt %l2, %l1
	//   br %tmp, PrefixMatch.end, PrefixMatch.cmp2
	//
	// PrefixMatch.cmp2:
	//   %tmp2 = call strncasecmp, %l1, %left, %l1, %right
	//   %tmp3 = cmp ne %tmp2, 0
	//   br %tmp3, PrefixMatch.end, PrefixMatch.ok
	//
	// PrefixMatch.ok:
	//   store 1, %result
	//   br PrefixMatch.end
	//
	// PrefixMatch.end:
	//   return %result
	// }}}

	llvm::Function *caller = builder_.GetInsertBlock()->getParent();
	llvm::IRBuilder<> ebb(&caller->getEntryBlock(), caller->getEntryBlock().begin());
	llvm::BasicBlock *cmp2BB = llvm::BasicBlock::Create(cx_, "PrefixMatch.cmp2");
	llvm::BasicBlock *okBB = llvm::BasicBlock::Create(cx_, "PrefixMatch.ok");
	llvm::BasicBlock *endBB = llvm::BasicBlock::Create(cx_, "PrefixMatch.end");

	llvm::Value *l1 = emitLoadStringLength(left);
	llvm::Value *l2 = emitLoadStringLength(right);
	llvm::Value *result = ebb.CreateAlloca(boolType(), NULL, "PrefixMatch.result.ptr");
	builder_.CreateStore(llvm::ConstantInt::get(boolType(), 0), result);

	llvm::Value *tmp = builder_.CreateICmpUGT(l2, l1);
	builder_.CreateCondBr(tmp, endBB, cmp2BB);

	// PrefixMatch.cmp2:
	caller->getBasicBlockList().push_back(cmp2BB);
	builder_.SetInsertPoint(cmp2BB);
	llvm::Value *v1 = emitLoadStringBuffer(left);
	llvm::Value *v2 = emitLoadStringBuffer(right);
	llvm::Value *tmp2 = emitCmpString(l2, v1, l2, v2);
	llvm::Value *tmp3 = builder_.CreateICmpNE(tmp2, llvm::ConstantInt::get(tmp2->getType(), 0));
	builder_.CreateCondBr(tmp3, endBB, okBB);

	// SuffixMatch.ok:
	caller->getBasicBlockList().push_back(okBB);
	builder_.SetInsertPoint(okBB);
	builder_.CreateStore(llvm::ConstantInt::get(boolType(), 1), result);
	builder_.CreateBr(endBB);

	// SuffixMatch.end:
	caller->getBasicBlockList().push_back(endBB);
	builder_.SetInsertPoint(endBB);
	return builder_.CreateLoad(result, "PrefixMatch.result");
}

llvm::Value *FlowRunner::emitSuffixMatch(llvm::Value *left, llvm::Value *right)
{
#if 1 == 0 // {{{ C equivalent
	size_t ll = strlen(left);
	size_t lr = strlen(right);

	if (lr > ll)
		return false;

	if (strcasecmp(left + (ll - lr), right) != 0)
		return false;

	return true;
#endif // }}}

	// {{{ IR
	//   %l1 = strlen %left
	//   %l2 = strlen %right
	//   %result = alloca i1
	//   store 0, %result
	//
	//   %tmp = cmp gt %l2, %l1
	//   br %tmp, SuffixMatch.end, SuffixMatch.cmp2
	//
	// SuffixMatch.cmp2:
	//   %ofs = sub %l1, %l2
	//   %lp = getelementptr inbounds %left, %ofs
	//   %tmp2 = call i1 strncasecmp, l2, %lp, l2, right
	//   %tmp3 = cmp ne %tmp2, 0
	//   br %tmp3, SuffixMatch.end, SuffixMatch.ok
	//
	// SuffixMatch.ok:
	//   store 1, %result
	//   br SuffixMatch.end
	//
	// SuffixMatch.end:
	//   return %result
	// }}}

	llvm::Function *caller = builder_.GetInsertBlock()->getParent();
	llvm::IRBuilder<> ebb(&caller->getEntryBlock(), caller->getEntryBlock().begin());
	llvm::BasicBlock *cmp2BB = llvm::BasicBlock::Create(cx_, "SuffixMatch.cmp2");
	llvm::BasicBlock *okBB = llvm::BasicBlock::Create(cx_, "SuffixMatch.ok");
	llvm::BasicBlock *endBB = llvm::BasicBlock::Create(cx_, "SuffixMatch.end");

	llvm::Value *l1 = emitLoadStringLength(left);
	llvm::Value *l2 = emitLoadStringLength(right);
	llvm::Value *result = ebb.CreateAlloca(boolType(), NULL, "SuffixMatch.result.ptr");
	builder_.CreateStore(llvm::ConstantInt::get(boolType(), 0), result);

	llvm::Value *tmp = builder_.CreateICmpUGT(l2, l1);
	builder_.CreateCondBr(tmp, endBB, cmp2BB);

	// SuffixMatch.cmp2:
	caller->getBasicBlockList().push_back(cmp2BB);
	builder_.SetInsertPoint(cmp2BB);
	llvm::Value *ofs = builder_.CreateSub(l1, l2, "ofs");
	llvm::Value *v1 = emitLoadStringBuffer(left);
	v1 = builder_.CreateInBoundsGEP(v1, ofs, "v1");
	llvm::Value *v2 = emitLoadStringBuffer(right);
	llvm::Value *tmp2 = emitCmpString(l2, v1, l2, v2);
	llvm::Value *tmp3 = builder_.CreateICmpNE(tmp2, llvm::ConstantInt::get(tmp2->getType(), 0));
	builder_.CreateCondBr(tmp3, endBB, okBB);

	// SuffixMatch.ok:
	caller->getBasicBlockList().push_back(okBB);
	builder_.SetInsertPoint(okBB);
	builder_.CreateStore(llvm::ConstantInt::get(boolType(), 1), result);
	builder_.CreateBr(endBB);

	// SuffixMatch.end:
	caller->getBasicBlockList().push_back(endBB);
	builder_.SetInsertPoint(endBB);
	return builder_.CreateLoad(result, "SuffixMatch.result");
}
// }}}

// {{{ expressions
void FlowRunner::visit(UnaryExpr& expr)
{
	FNTRACE();
	if (!codegen(expr.subExpr()))
		return;

	switch (expr.operatorStyle())
	{
		case Operator::Not:
		{
			if (value_->getType()->isIntegerTy()) {
				value_ = builder_.CreateICmpEQ(value_, llvm::ConstantInt::get(value_->getType(), 0), "cmp.not.i");
			} else if (isString(value_)) {
				llvm::Value *slen = emitLoadStringLength(value_);
				value_ = builder_.CreateICmpEQ(slen,
						llvm::ConstantInt::get(slen->getType(), 0), "cmp.not.str");
			} else if (isArray(value_)) {
				llvm::Value *alen = emitLoadArrayLength(value_);
				value_ = builder_.CreateICmpEQ(alen,
						llvm::ConstantInt::get(alen->getType(), 0), "cmp.not.ary");
			} else
				reportError("Invalid sub-type in not-expression");
			break;
		}
		case Operator::UnaryMinus:
			builder_.CreateNeg(value_);
			break;
		case Operator::UnaryPlus:
			break;
		default:
			reportError("Unknown operator style (%d) in unary operator", (int)expr.operatorStyle());
			break;
	}
}

void FlowRunner::visit(BinaryExpr& expr)
{
	FNTRACE();

	requestingLvalue_ = expr.operatorStyle() == Operator::Assign;

	llvm::Value *left = codegen(expr.leftExpr());

	switch (expr.operatorStyle())
	{
		case Operator::And:
			value_ = builder_.CreateAnd(toBool(left), toBool(codegen(expr.rightExpr())));
			return;
		case Operator::Or:
		{
			llvm::Function *caller = builder_.GetInsertBlock()->getParent();
			llvm::BasicBlock *rhsBB = llvm::BasicBlock::Create(cx_, "or.rhs", caller);
			llvm::BasicBlock *contBB = llvm::BasicBlock::Create(cx_, "or.cont");

			// cast lhs to bool
			left = toBool(left);
			builder_.CreateCondBr(left, contBB, rhsBB);
			llvm::BasicBlock *cmpBB = builder_.GetInsertBlock();

			// rhs-bb
			builder_.SetInsertPoint(rhsBB);
			llvm::Value *right = toBool(codegen(expr.rightExpr()));
			builder_.CreateBr(contBB);
			rhsBB = builder_.GetInsertBlock();

			// cont-bb
			caller->getBasicBlockList().push_back(contBB);
			builder_.SetInsertPoint(contBB);

			llvm::PHINode *pn = builder_.CreatePHI(llvm::Type::getInt1Ty(cx_));
			pn->addIncoming(left, cmpBB);
			pn->addIncoming(right, rhsBB);

			value_ = pn;
			return;
		}
		case Operator::Xor:
			value_ = builder_.CreateXor(toBool(left), toBool(codegen(expr.rightExpr())));
			return;
		default:
			break; // continue below
	}

	requestingLvalue_ = false;
	llvm::Value *right = codegen(expr.rightExpr());

	switch (expr.operatorStyle())
	{
		case Operator::Assign:
			value_ = builder_.CreateStore(right, left);
			break;
		case Operator::Plus:
			if (left->getType()->isFloatTy() && right->getType()->isFloatTy()) {
				// (fp, fp)
				value_ = builder_.CreateAdd(left, right);
			} else if (left->getType()->isIntegerTy() && right->getType()->isIntegerTy()) {
				// (int, int)
				value_ = builder_.CreateAdd(left, right);
			} else if (isCString(left) && right->getType()->isIntegerTy()) {
				// (string, int)
				value_ = builder_.CreateInBoundsGEP(left, right, "str.offset.l");
			} else if (isBufferPtr(left) && right->getType()->isIntegerTy()) {
				// (buffer, int)
				llvm::Value *len = emitLoadStringLength(left);
				llvm::Value *data = emitLoadStringBuffer(left);
				len = builder_.CreateSub(len, right);
				data = builder_.CreateInBoundsGEP(data, right);
				value_ = emitAllocaBuffer(len, data, "nbufref");

			} else if (isArray(left) && isArray(right)) {
				// (array, array)
				llvm::Value *nl = emitLoadArrayLength(left);
				llvm::Value *nr = emitLoadArrayLength(right);
				llvm::Value *n = builder_.CreateAdd(nl, nr);
				llvm::Value *result = builder_.CreateAlloca(valueType_, n, "result.array");
				emitCoreCall(CF::arrayadd, result, left, right);
				value_ = result;
			} else if (isStringPair(left, right)) {
				// (str, str)
				value_ = emitStringCat(left, right);
			} else
				reportError("operand types not compatible to operator +");
			break;
		case Operator::Minus:
			if (left->getType()->isIntegerTy() && right->getType()->isIntegerTy()) {
				// (int, int)
				value_ = builder_.CreateSub(left, right);
			} else if (isBufferPtr(left) && right->getType()->isIntegerTy()) {
				// (buffer, int)
				llvm::Value *len = emitLoadStringLength(left);
				llvm::Value *ofs = builder_.CreateSub(len, right);
				llvm::Value *data = emitLoadStringBuffer(left);
				data = builder_.CreateInBoundsGEP(data, ofs, "str.offset.l");
				value_ = emitAllocaBuffer(right, data, "nbufref");
			} else if (isCString(left) && right->getType()->isIntegerTy()) {
				// (string, int)
				llvm::Value *len = emitCoreCall(CF::strlen, left);
				llvm::Value *ofs = builder_.CreateSub(len, right);
				value_ = builder_.CreateInBoundsGEP(left, ofs, "str.offset.l");
			} else
				reportError("operand types not compatible to operator -");
			break;
		case Operator::Mul:
			if (left->getType()->isIntegerTy() && right->getType()->isIntegerTy()) {
				value_ = builder_.CreateMul(left, right);
			} else
				// TODO (string, int)
				// TODO (int, string)
				reportError("operand types not compatible to operator *");
			break;
		case Operator::Div:
			value_ = builder_.CreateSDiv(left, right);
			break;
		case Operator::Equal:
			if (isBool(left) && isBool(right)) {
				// (bool, bool)
				value_ = builder_.CreateICmpEQ(left, right);
			} else if (left->getType()->isIntegerTy() && right->getType()->isIntegerTy()) {
				// (int, int)
				if (static_cast<const llvm::IntegerType *>(left->getType())->getBitWidth() < 64)
					left = builder_.CreateIntCast(left, numberType(), false, "lhs.i64cast");

				if (static_cast<const llvm::IntegerType *>(right->getType())->getBitWidth() < 64)
					right = builder_.CreateIntCast(right, numberType(), false, "rhs.i64cast");

				value_ = builder_.CreateICmpEQ(left, right);
			} else if (isStringPair(left, right)) {
				// (string, string)
				value_ = emitCmpString(expr.operatorStyle(), left, right);
			} else if (isString(left) && right->getType()->isIntegerTy()) {
				// (string, int)
				left = emitLoadStringLength(left);
				value_ = builder_.CreateICmpEQ(left, right, "cmp.str.len");
			} else if (isArray(left) && isArray(right)) {
				// (array, array)
				value_ = emitCoreCall(CF::arraycmp, left, right);
				value_ = builder_.CreateICmpEQ(value_, llvm::ConstantInt::get(int32Type(), 0));
			} else if (isIPAddress(left) && isString(right)) {
				value_ = emitCoreCall(CF::ipstrcmp, left, right);
				value_ = builder_.CreateICmpEQ(value_, llvm::ConstantInt::get(int32Type(), 0));
			} else if (isIPAddress(left) && isIPAddress(right)) {
				value_ = emitCoreCall(CF::ipcmp, left, right);
				value_ = builder_.CreateICmpEQ(value_, llvm::ConstantInt::get(int32Type(), 0));
			} else {
				reportError("Incompatible operand types for operator ==");
				printf("left:\n");
				left->dump();
				printf("right:\n");
				right->dump();
			}
			break;
		case Operator::UnEqual:
			if (left->getType()->isIntegerTy() && right->getType()->isIntegerTy()) {
				// (int, int)
				value_ = builder_.CreateICmpNE(left, right, "cmp.dd");
			} else if (isBool(left) && isBool(right)) {
				// (bool, bool)
				value_ = builder_.CreateICmpNE(left, right);
			} else if (isStringPair(left, right)) {
				// (str, str)
				value_ = emitCmpString(expr.operatorStyle(), left, right);
			} else if (isString(left) && right->getType()->isIntegerTy()) {
				// (string, int)
				left = emitLoadStringLength(left);
				value_ = builder_.CreateICmpNE(left, right, "cmp.str.len");
			} else if (isArray(left) && isArray(right)) {
				// (array, array)
				value_ = emitCoreCall(CF::arraycmp, left, right);
				value_ = builder_.CreateICmpNE(value_, llvm::ConstantInt::get(int32Type(), 0));
			} else if (isIPAddress(left) && isString(right)) {
				value_ = emitCoreCall(CF::ipstrcmp, left, right);
				value_ = builder_.CreateICmpNE(value_, llvm::ConstantInt::get(int32Type(), 0));
			} else if (isIPAddress(left) && isIPAddress(right)) {
				value_ = emitCoreCall(CF::ipcmp, left, right);
				value_ = builder_.CreateICmpNE(value_, llvm::ConstantInt::get(int32Type(), 0));
			} else {
				reportError("Incompatible operand types for operator !=");
				printf("left:\n");
				left->dump();
				printf("right:\n");
				right->dump();
			}
			break;
		case Operator::Less:
			if (left->getType()->isIntegerTy() && right->getType()->isIntegerTy()) {
				// (int, int)
				value_ = builder_.CreateICmpSLT(left, right);
			} else if (isStringPair(left, right)) {
				// (str, str)
				value_ = emitCmpString(expr.operatorStyle(), left, right);
			} else if (isString(left) && right->getType()->isIntegerTy()) {
				// (string, int)
				left = emitLoadStringLength(left);
				value_ = builder_.CreateICmpULT(left, right, "cmp.str.len");
			} else {
				reportError("Incompatible operand types for operator <");
			}
			break;
		case Operator::Greater:
			if (left->getType()->isIntegerTy() && right->getType()->isIntegerTy()) {
				// (int, int)
				value_ = builder_.CreateICmpSGT(left, right);
			} else if (isStringPair(left, right)) {
				// (str, str)
				value_ = emitCmpString(expr.operatorStyle(), left, right);
			} else if (isString(left) && right->getType()->isIntegerTy()) {
				// (string, int)
				left = emitLoadStringLength(left);
				value_ = builder_.CreateICmpUGT(left, right, "cmp.str.len");
			} else {
				reportError("Incompatible operand types for operator >");
			}
			break;
		case Operator::LessOrEqual:
			if (left->getType()->isIntegerTy() && right->getType()->isIntegerTy()) {
				// (int, int)
				value_ = builder_.CreateICmpSLE(left, right);
			} else if (isStringPair(left, right)) {
				// (str, str)
				value_ = emitCmpString(expr.operatorStyle(), left, right);
			} else if (isString(left) && right->getType()->isIntegerTy()) {
				// (string, int)
				left = emitLoadStringLength(left);
				value_ = builder_.CreateICmpSLE(left, right, "cmp.str.len");
			} else {
				reportError("Incompatible operand types for operator <=");
			}
			break;
		case Operator::GreaterOrEqual:
			if (left->getType()->isIntegerTy() && right->getType()->isIntegerTy()) {
				// (int, int)
				value_ = builder_.CreateICmpSGE(left, right);
			} else if (isString(left) && right->getType()->isIntegerTy()) {
				// (string, int)
				left = emitLoadStringLength(left);
				value_ = builder_.CreateICmpSGE(left, right, "cmp.str.len");
			} else if (isStringPair(left, right)) {
				// (str, str)
				value_ = emitCmpString(expr.operatorStyle(), left, right);
			} else {
				reportError("Incompatible operand types for operator >=");
			}
			break;
		case Operator::PrefixMatch:
			if (isStringPair(left, right)) {
				// (string, string)
				value_ = emitPrefixMatch(left, right);
			} else {
				reportError("Incompatible operand types for operator =^");
			}
			break;
		case Operator::SuffixMatch:
			// 'foobar' =$ 'bar'
			if (isStringPair(left, right)) {
				// (str, str)
				value_ = emitSuffixMatch(left, right);
			} else {
				reportError("Incompatible operand types for operator =$");
			}
			break;
		case Operator::RegexMatch:
			// VALUE =~ PATTERN
			if (isString(left) && isString(right)) {
				value_ = emitCmpString(Operator::RegexMatch, left, right);
			} else if (isString(left) && isRegExp(right)) {
				llvm::Value *len = emitLoadStringLength(left);
				llvm::Value *buf = emitLoadStringBuffer(left);
				value_ = emitCoreCall(CF::regexmatch2, len, buf, right);
			} else {
				reportError("Incompatible operand types for operator =~");
			}
			break;
		case Operator::In:
			// EXPR 'in' EXPR
			if (isString(left) && isString(right)) {
				// (string, string)
				value_ = emitIsSubString(right, left);
			} else if (isNumber(left) && isArray(right)) {
				// (number, array)
				value_ = emitCoreCall(CF::NumberInArray, left, right);
				value_ = builder_.CreateICmpNE(value_, llvm::ConstantInt::get(value_->getType(), 0));
			} else if (isString(left) && isArray(right)) {
				// (string, array)
				llvm::Value *len = emitLoadStringLength(left);
				llvm::Value *buf = emitLoadStringBuffer(left);
				value_ = emitCoreCall(CF::StringInArray, len, buf, right);
				value_ = builder_.CreateICmpNE(value_, llvm::ConstantInt::get(value_->getType(), 0));
			} else {
				reportError("Incompatible operand types for operator: 'in'");
			}
			break;
		case Operator::Pow:
			if (left->getType()->isIntegerTy() && right->getType()->isIntegerTy()) {
				left = builder_.CreateSIToFP(left, doubleType());
				right = builder_.CreateSIToFP(right, doubleType());
				value_ = builder_.CreateFPToSI(emitCoreCall(CF::pow, left, right), numberType());
			} else {
				reportError("Incompatible operand types for operator **");
			}
			break;
		default:
			reportError("unsupported binary-operator op (%d)\n", (int)expr.operatorStyle());
			break;
	}
}

void FlowRunner::visit(StringExpr& expr)
{
	FNTRACE();

	value_ = builder_.CreateGlobalStringPtr(expr.value().c_str());
	//value_ = emitGlobalString(expr.value());
}

void FlowRunner::visit(NumberExpr& expr)
{
	FNTRACE();

	value_ = llvm::ConstantInt::get(numberType(), expr.value());
}

void FlowRunner::visit(BoolExpr& expr)
{
	FNTRACE();

	value_ = llvm::ConstantInt::get(numberType(), expr.value() ? 1 : 0);
}

void FlowRunner::visit(RegExpExpr& expr)
{
	FNTRACE();

	//printf("runner.visit(regexpexpr&)\n");
	const RegExp *re = & expr.value();
	value_ = llvm::ConstantInt::get(int64Type(), (int64_t)(re));
	value_ = builder_.CreateIntToPtr(value_, regexpType_->getPointerTo());
}

void FlowRunner::visit(IPAddressExpr& expr)
{
	FNTRACE();

	const IPAddress *ipaddr = & expr.value();
	value_ = llvm::ConstantInt::get(int64Type(), (int64_t)(ipaddr));
	value_ = builder_.CreateIntToPtr(value_, ipaddrType_->getPointerTo());
}

void FlowRunner::visit(VariableExpr& expr)
{
	FNTRACE();

	value_ = codegen(expr.variable());
	if (!value_)
		return;

	if (expr.variable()->parentScope()
		&& expr.variable()->parentScope()->outerTable() != NULL)
	{
		//printf("local variable in expression: '%s'\n", expr.variable()->name().c_str());
		if (!requestingLvalue_)
			value_ = builder_.CreateLoad(value_, expr.variable()->name().c_str());
	}
	else
	{
		//printf("global variable in expression: '%s'\n", expr.variable()->name().c_str());
	}
}

void FlowRunner::visit(FunctionRefExpr& expr)
{
	FNTRACE();

	value_ = codegen(expr.function());
}

void FlowRunner::visit(CallExpr& call)
{
	FNTRACE();

	int id = findNative(call.callee()->name());
	if (id == -1)
		emitCall(call.callee(), call.args());
	else
	{
		if (call.callStyle() == CallExpr::Assignment && !backend_->isVariable(call.callee()->name()))
		{
			reportError("Trying to assign a value to non-variable '%s'", call.callee()->name().c_str());
			return;
		}

		emitNativeCall(id, call.args());
	}
}

llvm::Value *FlowRunner::emitCoreCall(CF id, llvm::Value *p1)
{
	llvm::Function *calleeFn = coreFunctions_[static_cast<int>(id)];
	llvm::Value *p[1] = { p1 };
	return value_ = builder_.CreateCall(calleeFn, p, p + sizeof(p) / sizeof(*p));
}

llvm::Value *FlowRunner::emitCoreCall(CF id, llvm::Value *p1, llvm::Value *p2)
{
	llvm::Function *calleeFn = coreFunctions_[static_cast<int>(id)];
	llvm::Value *p[2] = { p1, p2 };
	return value_ = builder_.CreateCall(calleeFn, p, p + sizeof(p) / sizeof(*p));
}

llvm::Value *FlowRunner::emitCoreCall(CF id, llvm::Value *p1, llvm::Value *p2, llvm::Value *p3)
{
	llvm::Function *calleeFn = coreFunctions_[static_cast<int>(id)];
	llvm::Value *p[3] = { p1, p2, p3 };
	return value_ = builder_.CreateCall(calleeFn, p, p + sizeof(p) / sizeof(*p));
}

llvm::Value *FlowRunner::emitCoreCall(CF id, llvm::Value *p1, llvm::Value *p2, llvm::Value *p3, llvm::Value *p4)
{
	llvm::Function *calleeFn = coreFunctions_[static_cast<int>(id)];
	llvm::Value *p[4] = { p1, p2, p3, p4 };
	return value_ = builder_.CreateCall(calleeFn, p, p + sizeof(p) / sizeof(*p));
}

/** emits a non-native function call (if function is a handler do handle the result, too).
 *
 * \param callee the function to call.
 * \param callArgs the function parameters to pass to the callee.
 */
void FlowRunner::emitCall(Function *callee, ListExpr *callArgs)
{
	llvm::Function *callerFn = builder_.GetInsertBlock()->getParent();
	llvm::Function *calleeFn = module_->getFunction(callee->name());

	// In case the invoked callee has not yet been emitted to LLVM, do it now.
	if (!calleeFn)
	{
		codegen(callee);

		calleeFn = module_->getFunction(callee->name());

		if (!calleeFn)
		{
			// an error occured during code generation, and it must have been
			// already reported by the codegen call above.
			return;
		}
	}

	std::vector<llvm::Value *> args;

	if (callee->isHandler())
		args.push_back(scope_.lookup(NULL)); // context userdata (id: NULL)

	if (callArgs)
	{
		for (auto i = callArgs->begin(), e = callArgs->end(); i != e; ++i)
			args.push_back(codegen(*i));
	}

	value_ = builder_.CreateCall(calleeFn, args.begin(), args.end());

	if (callee->isHandler())
	{
		// handlers MUST NOT occur within expressions itself, just within ExprStmt,
		// though, evaluate its result code, and return to caller if result is true,
		// that is, callee *handled* the request.

		llvm::Value *condValue = value_;
		llvm::BasicBlock *doneBlock = llvm::BasicBlock::Create(cx_, "handler.done", callerFn);
		llvm::BasicBlock *contBlock = llvm::BasicBlock::Create(cx_, "handler.cont");
		builder_.CreateCondBr(condValue, doneBlock, contBlock);

		// emit handler.then block
		builder_.SetInsertPoint(doneBlock);
		builder_.CreateRet(llvm::ConstantInt::get(llvm::Type::getInt1Ty(cx_), 1));
		doneBlock = builder_.GetInsertBlock();

		// emit handler.end block
		callerFn->getBasicBlockList().push_back(contBlock);
		builder_.SetInsertPoint(contBlock);
	}
}

void FlowRunner::visit(ListExpr& expr)
{
	FNTRACE();

	size_t nelems = expr.length();

	llvm::Value *array = builder_.CreateAlloca(valueType_,
		llvm::ConstantInt::get(llvm::Type::getInt32Ty(cx_), nelems + 1), "list.ptr");

	for (size_t i = 0; i != nelems; ++i)
	{
		llvm::Value *value = codegen(expr.at(i));
		emitNativeValue(i, array, value);
	}
	emitNativeValue(nelems, array, NULL);

	value_ = array;
}

void FlowRunner::visit(ExprStmt& stmt)
{
	FNTRACE();

	codegen(stmt.expression());
}
// }}}

// {{{ statements
void FlowRunner::visit(CompoundStmt& stmt)
{
	FNTRACE();

	for (auto s: stmt)
		codegen(s);

	value_ = NULL; // XXX do we have some *single* thing we could/should treat as result value_?
}

void FlowRunner::visit(CondStmt& stmt)
{
	FNTRACE();

	llvm::Function *caller = builder_.GetInsertBlock()->getParent();

	llvm::Value *condValue = toBool(codegen(stmt.condition()));

	llvm::BasicBlock *thenBlock = llvm::BasicBlock::Create(cx_, "on.then", caller);
	llvm::BasicBlock *elseBlock = llvm::BasicBlock::Create(cx_, "on.else");
	llvm::BasicBlock *contBlock = llvm::BasicBlock::Create(cx_, "on.cont");
	builder_.CreateCondBr(condValue, thenBlock, elseBlock);

	// emit on.then block
	builder_.SetInsertPoint(thenBlock);
	codegen(stmt.thenStmt());
	builder_.CreateBr(contBlock);
	thenBlock = builder_.GetInsertBlock();

	// emit on.else block
	caller->getBasicBlockList().push_back(elseBlock);
	builder_.SetInsertPoint(elseBlock);
	codegen(stmt.elseStmt());
	builder_.CreateBr(contBlock);

	// emit on.cont block
	caller->getBasicBlockList().push_back(contBlock);
	builder_.SetInsertPoint(contBlock);
}
// }}}

} // namespace x0