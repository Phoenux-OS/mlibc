
#include <elf.h>

#include <frg/manual_box.hpp>

#include <abi-bits/auxv.h>
#include <mlibc/debug.hpp>
#include <mlibc/sysdeps.hpp>
#include "linker.hpp"

#define HIDDEN  __attribute__ ((visibility ("hidden")))
#define EXPORT  __attribute__ ((visibility ("default")))

static constexpr bool logEntryExit = false;
static constexpr bool logStartup = false;

extern HIDDEN void *_GLOBAL_OFFSET_TABLE_[];
extern HIDDEN Elf64_Dyn _DYNAMIC[];

uintptr_t *entryStack;
frg::manual_box<ObjectRepository> initialRepository;
frg::manual_box<Scope> globalScope;
frg::manual_box<Loader> globalLoader;

frg::manual_box<RuntimeTlsMap> runtimeTlsMap;

// Relocates the dynamic linker (i.e. this DSO) itself.
// Assumptions:
// - There are no references to external symbols.
// Note that this code is fragile in the sense that it must not contain relocations itself.
// TODO: Use tooling to verify this at compile time.
extern "C" void relocateSelf() {
	size_t rela_offset = 0;
	size_t rela_size = 0;
	for(size_t i = 0; _DYNAMIC[i].d_tag != DT_NULL; i++) {
		auto ent = &_DYNAMIC[i];
		switch(ent->d_tag) {
		case DT_RELA: rela_offset = ent->d_ptr; break;
		case DT_RELASZ: rela_size = ent->d_val; break;
		}
	}
	
	auto ldso_base = reinterpret_cast<uintptr_t>(_DYNAMIC)
			- reinterpret_cast<uintptr_t>(_GLOBAL_OFFSET_TABLE_[0]);
	for(size_t disp = 0; disp < rela_size; disp += sizeof(Elf64_Rela)) {
		auto reloc = reinterpret_cast<Elf64_Rela *>(ldso_base + rela_offset + disp);

		Elf64_Xword type = ELF64_R_TYPE(reloc->r_info);
		if(ELF64_R_SYM(reloc->r_info))
			__builtin_trap();

		auto p = reinterpret_cast<uint64_t *>(ldso_base + reloc->r_offset);
		switch(type) {
		case R_X86_64_RELATIVE:
			*p = ldso_base + reloc->r_addend;
			break;
		default:
			__builtin_trap();
		}
	}
}

extern "C" void *lazyRelocate(SharedObject *object, unsigned int rel_index) {
	__ensure(object->lazyExplicitAddend);
	auto reloc = (Elf64_Rela *)(object->baseAddress + object->lazyRelocTableOffset
			+ rel_index * sizeof(Elf64_Rela));
	Elf64_Xword type = ELF64_R_TYPE(reloc->r_info);
	Elf64_Xword symbol_index = ELF64_R_SYM(reloc->r_info);

	__ensure(type == R_X86_64_JUMP_SLOT);

	auto symbol = (Elf64_Sym *)(object->baseAddress + object->symbolTableOffset
			+ symbol_index * sizeof(Elf64_Sym));
	ObjectSymbol r(object, symbol);
	frg::optional<ObjectSymbol> p = object->loadScope->resolveSymbol(r, 0);
	if(!p)
		mlibc::panicLogger() << "Unresolved JUMP_SLOT symbol" << frg::endlog;

	//mlibc::infoLogger() << "Lazy relocation to " << symbol_str
	//		<< " resolved to " << pointer << frg::endlog;
	
	*(uint64_t *)(object->baseAddress + reloc->r_offset) = p->virtualAddress();
	return (void *)p->virtualAddress();
}

extern "C" [[ gnu::visibility("default") ]] void __rtdl_setupTcb() {
	allocateTcb();
}

extern "C" void *interpreterMain(uintptr_t *entry_stack) {
	if(logEntryExit)
		mlibc::infoLogger() << "Entering ld.so" << frg::endlog;
	entryStack = entry_stack;
	runtimeTlsMap.initialize();
	
	auto ldso_base = reinterpret_cast<uintptr_t>(_DYNAMIC)
			- reinterpret_cast<uintptr_t>(_GLOBAL_OFFSET_TABLE_[0]);
	if(logStartup) {
		mlibc::infoLogger() << "ldso: Own base address is: 0x"
				<< frg::hex_fmt(ldso_base) << frg::endlog;
		mlibc::infoLogger() << "ldso: Own dynamic section is at: " << _DYNAMIC << frg::endlog;
	}

	// TODO: Use a fake PLT stub that reports an error message?
	_GLOBAL_OFFSET_TABLE_[1] = 0;
	_GLOBAL_OFFSET_TABLE_[2] = 0;
	
	// Validate our own dynamic section.
	// Here, we make sure that the dynamic linker does not need relocations itself.
	uintptr_t strtab_offset = 0;
	uintptr_t soname_str = 0;
	for(size_t i = 0; _DYNAMIC[i].d_tag != DT_NULL; i++) {
		auto ent = &_DYNAMIC[i];
		switch(ent->d_tag) {
		case DT_STRTAB: strtab_offset = ent->d_ptr; break;
		case DT_SONAME: soname_str = ent->d_val; break;
		case DT_HASH:
		case DT_GNU_HASH:
		case DT_STRSZ:
		case DT_SYMTAB:
		case DT_SYMENT:
		case DT_RELA:
		case DT_RELASZ:
		case DT_RELAENT:
		case DT_RELACOUNT:
			continue;
		default:
			__ensure(!"Unexpected dynamic entry in program interpreter");
		}
	}
	__ensure(strtab_offset);
	__ensure(soname_str);
	
	void *phdr_pointer = 0;
	size_t phdr_entry_size = 0;
	size_t phdr_count = 0;
	void *entry_pointer = 0;

	// Find the auxiliary vector by skipping args and environment.
	auto aux = entryStack;
	aux += *aux + 1; // First, we skip argc and all args.
	__ensure(!*aux);
	aux++;
	while(*aux) // Now, we skip the environment.
		aux++;
	aux++;

	// Parse the actual vector.
	while(true) {
		auto value = aux + 1;
		if(!(*aux))
			break;
		
		// TODO: Whitelist auxiliary vector entries here?
		switch(*aux) {
			case AT_PHDR: phdr_pointer = reinterpret_cast<void *>(*value); break;
			case AT_PHENT: phdr_entry_size = *value; break;
			case AT_PHNUM: phdr_count = *value; break;
			case AT_ENTRY: entry_pointer = reinterpret_cast<void *>(*value); break;
		}

		aux += 2;
	}
	__ensure(phdr_pointer);
	__ensure(entry_pointer);

	if(logStartup)
		mlibc::infoLogger() << "ldso: Executable PHDRs are at " << phdr_pointer
				<< frg::endlog;

	// perform the initial dynamic linking
	initialRepository.initialize();

	globalScope.initialize();

	// Add the dynamic linker, as well as the exectuable to the repository.
	auto ldso_soname = reinterpret_cast<const char *>(ldso_base + strtab_offset + soname_str);
	initialRepository->injectObjectFromDts(ldso_soname, ldso_base, _DYNAMIC, 1);

	// TODO: support non-zero base addresses?
	auto executable = initialRepository->injectObjectFromPhdrs("(executable)", phdr_pointer,
			phdr_entry_size, phdr_count, entry_pointer, 1);

	Loader linker{globalScope.get(), true, 1};
	linker.submitObject(executable);
	linker.linkObjects();
	allocateTcb();
	linker.initObjects();

	if(logEntryExit)
		mlibc::infoLogger() << "Leaving ld.so, jump to "
				<< (void *)executable->entry << frg::endlog;
	return executable->entry;
}

// the layout of this structure is dictated by the ABI
struct __abi_tls_entry {
	SharedObject *object;
	uint64_t offset;
};

//static_assert(sizeof(__abi_tls_entry) == 16, "Bad __abi_tls_entry size");

const char *lastError;

extern "C" [[ gnu::visibility("default") ]] uintptr_t *__dlapi_entrystack() {
	return entryStack;
}

extern "C" [[gnu::visibility("default")]]
const char *__dlapi_error() {
	auto error = lastError;
	lastError = nullptr;
	return error;
}

extern "C" __attribute__ (( visibility("default") ))
void *__dlapi_get_tls(struct __abi_tls_entry *entry) {
	// TODO: Thread-safety!
	__ensure(entry->object->tlsModel == TlsModel::initial);
	
//	frigg::infoLogger() << "__tls_get_addr(" << entry->object->name
//			<< ", " << entry->offset << ")" << frg::endlog;
	
	char *tp;
	asm ( "mov %%fs:(0), %0" : "=r" (tp) );
	return tp + entry->object->tlsOffset + entry->offset;
}

extern "C" [[gnu::visibility("default")]]
void *__dlapi_open(const char *file, int local) {
	// TODO: Thread-safety!
	auto rts = rtsCounter++;
	
	if(local)
		mlibc::infoLogger() << "\e[31mrtdl: RTLD_LOCAL is not supported properly\e[39m"
				<< frg::endlog;

	SharedObject *object;
	if(frg::string_view{file}.find_first('/') == size_t(-1)) {
		object = initialRepository->requestObjectWithName(file, nullptr, rts);
	}else{
		object = initialRepository->requestObjectAtPath(file, rts);
	}
	if(!object) {
		lastError = "Cannot locate requested DSO";
		return nullptr;
	}

	Loader linker{globalScope.get(), false, rts};
	linker.submitObject(object);
	linker.linkObjects();
	linker.initObjects();

	// Build the object scope. TODO: Use the Loader object to do this.
	if(!object->objectScope) {
		struct Token { };

		using Set = frg::hash_map<SharedObject *, Token,
				frg::hash<SharedObject *>, MemoryAllocator>;
		Set set{frg::hash<SharedObject *>{}, getAllocator()};
		
		object->objectScope = frg::construct<Scope>(getAllocator());
		frg::vector<SharedObject *, MemoryAllocator> queue{getAllocator()};

		object->objectScope->appendObject(object);
		set.insert(object, Token{});
		queue.push(object);

		// Loop over indices (not iterators) here: We are adding elements in the loop!
		for(size_t i = 0; i < queue.size(); i++) {
			auto current = queue[i];
			if(set.get(current))
				continue;
		
			object->objectScope->appendObject(current);
			set.insert(current, Token{});
			queue.push(current);
		}
	}

	return object;
}

extern "C" [[gnu::visibility("default")]]
void *__dlapi_resolve(void *handle, const char *string) {
	mlibc::infoLogger() << "rtdl: __dlapi_resolve(" << string << ")" << frg::endlog;

	__ensure(handle != reinterpret_cast<void *>(-1));

	frg::optional<ObjectSymbol> target;
	if(handle) {
		auto object = reinterpret_cast<SharedObject *>(handle);
		__ensure(object->objectScope);
		target = Scope::resolveWholeScope(object->objectScope, string, 0);
	}else{
		target = Scope::resolveWholeScope(globalScope.get(), string, 0);
	}

	__ensure(target);
	return reinterpret_cast<void *>(target->virtualAddress());
}

struct __dlapi_symbol {
	const char *file;
	void *base;
	const char *symbol;
	void *address;
};

extern "C" [[gnu::visibility("default")]]
int __dlapi_reverse(const void *ptr, __dlapi_symbol *info) {
	mlibc::infoLogger() << "rtdl: __dlapi_reverse(" << ptr << ")" << frg::endlog;

	for(size_t i = 0; i < globalScope->_objects.size(); i++) {
		auto object = globalScope->_objects[i];
	
		auto eligible = [&] (ObjectSymbol cand) {
			if(cand.symbol()->st_shndx == SHN_UNDEF)
				return false;

			auto bind = ELF64_ST_BIND(cand.symbol()->st_info);
			if(bind != STB_GLOBAL && bind != STB_WEAK)
				return false;

			return true;
		};
	
		auto hash_table = (Elf64_Word *)(object->baseAddress + object->hashTableOffset);
		auto num_symbols = hash_table[1];
		for(size_t i = 0; i < num_symbols; i++) {
			ObjectSymbol cand{object, (Elf64_Sym *)(object->baseAddress
					+ object->symbolTableOffset + i * sizeof(Elf64_Sym))};
			if(eligible(cand) && cand.virtualAddress() == reinterpret_cast<uintptr_t>(ptr)) {
				mlibc::infoLogger() << "rtdl: Found symbol " << cand.getString() << " in object "
						<< object->name << frg::endlog;
				info->file = object->name;
				info->base = reinterpret_cast<void *>(object->baseAddress);
				info->symbol = cand.getString();
				info->address = reinterpret_cast<void *>(cand.virtualAddress());
				return 0;
			}
		}
	}

	mlibc::panicLogger() << "rtdl: Could not find symbol in __dlapi_reverse()" << frg::endlog;
	return -1;
}

