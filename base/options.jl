# This file is a part of Julia. License is MIT: https://julialang.org/license

# NOTE: This type needs to be kept in sync with jl_options in src/jloptions.h
struct JLOptions
    quiet::Int8
    banner::Int8
    julia_bindir::Ptr{UInt8}
    julia_bin::Ptr{UInt8}
    commands::Ptr{Ptr{UInt8}} # (e)eval, (E)print, (L)load
    image_file::Ptr{UInt8}
    cpu_target::Ptr{UInt8}
    nthreadpools::Int8
    nthreads::Int16
    nmarkthreads::Int16
    nsweepthreads::Int8
    nthreads_per_pool::Ptr{Int16}
    nprocs::Int32
    machine_file::Ptr{UInt8}
    project::Ptr{UInt8}
    program_file::Ptr{UInt8}
    isinteractive::Int8
    color::Int8
    historyfile::Int8
    startupfile::Int8
    compile_enabled::Int8
    code_coverage::Int8
    malloc_log::Int8
    tracked_path::Ptr{UInt8}
    opt_level::Int8
    opt_level_min::Int8
    debug_level::Int8
    check_bounds::Int8
    depwarn::Int8
    warn_overwrite::Int8
    can_inline::Int8
    polly::Int8
    trace_compile::Ptr{UInt8}
    trace_dispatch::Ptr{UInt8}
    fast_math::Int8
    worker::Int8
    cookie::Ptr{UInt8}
    handle_signals::Int8
    use_experimental_features::Int8
    use_sysimage_native_code::Int8
    use_compiled_modules::Int8
    use_pkgimages::Int8
    bindto::Ptr{UInt8}
    outputbc::Ptr{UInt8}
    outputunoptbc::Ptr{UInt8}
    outputo::Ptr{UInt8}
    outputasm::Ptr{UInt8}
    outputji::Ptr{UInt8}
    output_code_coverage::Ptr{UInt8}
    incremental::Int8
    image_file_specified::Int8
    warn_scope::Int8
    image_codegen::Int8
    rr_detach::Int8
    strip_metadata::Int8
    strip_ir::Int8
    permalloc_pkgimg::Int8
    heap_size_hint::UInt64
    hard_heap_limit::UInt64
    heap_target_increment::UInt64
    trace_compile_timing::Int8
    trim::Int8
    task_metrics::Int8
    timeout_for_safepoint_straggler_s::Int16
    gc_sweep_always_full::Int8
end

# This runs early in the sysimage != is not defined yet
if sizeof(JLOptions) === ccall(:jl_sizeof_jl_options, Int, ())
else
    ccall(:jl_throw, Cvoid, (Any,), "Option structure mismatch")
end

JLOptions() = unsafe_load(cglobal(:jl_options, JLOptions))

function colored_text(opts::JLOptions)
    return if opts.color != 0
        opts.color == 1
    elseif !isempty(get(ENV, "FORCE_COLOR", ""))
        true
    elseif !isempty(get(ENV, "NO_COLOR", ""))
        false
    else
        nothing
    end
end

function show(io::IO, opt::JLOptions)
    print(io, "JLOptions(")
    fields = fieldnames(JLOptions)
    nfields = length(fields)
    for (i, f) in enumerate(fields)
        v = getfield(opt, i)
        if isa(v, Ptr{UInt8})
            v = (v != C_NULL) ? unsafe_string(v) : ""
        elseif isa(v, Ptr{Ptr{UInt8}})
            v = unsafe_load_commands(v)
        end
        print(io, f, " = ", repr(v), i < nfields ? ", " : "")
    end
    print(io, ")")
end

function unsafe_load_commands(v::Ptr{Ptr{UInt8}})
    cmds = Pair{Char, String}[]
    v == C_NULL && return cmds
    i = 1
    while true
        s = unsafe_load(v, i)
        s == C_NULL && break
        e = Char(unsafe_load(s))
        push!(cmds, e => unsafe_string(s + 1))
        i += 1
    end
    return cmds
end

function is_file_tracked(file::Symbol)
    return ccall(:jl_is_file_tracked, Cint, (Any,), file) == 1
end
