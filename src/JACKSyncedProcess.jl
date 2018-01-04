
using Compat
using RingBuffers

include("libjack.jl")
include("jack_shim.jl")

notifycb(handle) = ccall(:uv_async_send, Cint, (Ptr{Void}, ), handle)
global const notifycb_c = cfunction(notifycb, Cint, (Ptr{Void}, ))

mutable struct JACKSyncedProcess
    shim_info::jack_shim_info_t    
    async_cond::Base.AsyncCondition
    process::Function
    clientptr::Ptr{Void}
    inptrs::Vector{Ptr{Void}}
    outptrs::Vector{Ptr{Void}}
    inbufs::Vector{PaUtilRingBuffer}
    outbufs::Vector{PaUtilRingBuffer}
    nframes::Integer
end

function JACKSyncedProcess(inports::Vector{String}, outports::Vector{String}, 
                           process::Function, nframes::Integer, name::String="jsptest")
    # create AsyncCondition
    async_cond = Base.AsyncCondition()

    # create client
    status = Ref{Cint}(Failure)
    clientptr = jack_client_open(name, 0, status)
    if C_NULL == clientptr
        error("Failure opening JACK Client: ", status_str(status[]))
    end

    # register inports
    inptrs = Vector{Ptr{Void}}(0)
    inbufs = Vector{PaUtilRingBuffer}(0)
    flags = 0
    for inport in inports
        portptr = jack_port_register(clientptr, inport, PortIsInput, flags, 0)
        push!(inptrs, portptr)
        if C_NULL == portptr
            error("Failure opening JACK Client: ", status_str(status[]))
        end

        portbuf = PaUtilRingBuffer(sizeof(Float32), nframes*4)
        push!(inbufs, portbuf)
    end

    # register outports
    outptrs = Vector{Ptr{Void}}(0)
    outbufs = Vector{PaUtilRingBuffer}(0)
    flags = 0
    for outport in outports
        portptr = jack_port_register(clientptr, outport, PortIsOutput, flags, 0)
        push!(outptrs, portptr)
        if C_NULL == portptr
            error("Failure opening JACK Client: ", status_str(status[]))
        end

        portbuf = PaUtilRingBuffer(sizeof(Float32), nframes*4)
        push!(outbufs, portbuf)
    end

    # create shim info
    shim_info = jack_shim_info_t(    # constructor will fill out empty arrays for
        [p for p in inports],        # jack_port_t *inports[JACK_SHIM_MAX_PORTS];
        [p for p in outports],       # jack_port_t *outports[JACK_SHIM_MAX_PORTS];
        [b.buffer for b in inbufs],  # PaUtilRingBuffer *inputbufs[JACK_SHIM_MAX_PORTS];
        [b.buffer for b in outbufs], # PaUtilRingBuffer *outputbufs[JACK_SHIM_MAX_PORTS];
        Ptr{PaUtilRingBuffer}(0),    # FIXME, errbuf...
        Cint(1),                     # synced
        length(inports),             # inputchans
        length(outports),            # outputchans
        notifycb_c,                  # notifycb...
        Ptr{Void}(0),                # inhandle...
        Ptr{Void}(0),                # outhandle...
        Ptr{Void}(0),                # errhandle...
        async_cond.handle            # synchandle...
    )

    JACKSyncedProcess(
        shim_info,  # ::jack_shim_info_t    
        async_cond, # ::Base.AsyncCondition
        process,    # ::Function
        clientptr,  # ::Ptr{Void}
        inptrs,     # ::Vector{Ptr{Void}}
        outptrs,    # ::Vector{Ptr{Void}}
        inbufs,     # ::Vector{PaUtilRingBuffer}
        outbufs,    # ::Vector{PaUtilRingBuffer}
    )
    
end

# fixme generic constructors with int channels
function JACKSyncedProcess(inchans::Integer, outchans::Integer, 
        process::Function, nframes::Integer)
    JACKSyncedProcess(
        [@sprintf("in_%02d",itm) for itm in 1:inchans], # Vector{String}
        [@sprintf("out_%02d",itm) for itm in 1:outchans], # Vector{String}
        process, nframes)
end



