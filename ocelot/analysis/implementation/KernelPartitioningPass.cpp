/*!
	\file KernelPartitioningPass.cpp
	\author Andrew Kerr <arkerr@gatech.edu>
	\date November 17, 2011
	\brief implements kernel partitioning
*/

// Boost includes
#include <boost/lexical_cast.hpp>

// Ocelot includes
#include <ocelot/ir/interface/Kernel.h>
#include <ocelot/analysis/interface/DataflowGraph.h>
#include <ocelot/analysis/interface/KernelPartitioningPass.h>

// Hydrazine includes
#include <hydrazine/implementation/debug.h>
#include <hydrazine/implementation/Exception.h>
#include <hydrazine/implementation/math.h>

//////////////////////////////////////////////////////////////////////////////////////////////////

#define Ocelot_Exception(x) { std::stringstream ss; ss << x; std::cerr << x << std::endl; \
	throw hydrazine::Exception(ss.str()); }
#ifdef REPORT_BASE
#undef REPORT_BASE
#endif
////////////////////////////////////////////////////////////////////////////////////////////////////

#define REPORT_BASE 1

#define REPORT_EMIT_SUBKERNEL_PTX 1
#define REPORT_EMIT_SOURCE_PTXKERNEL 1

////////////////////////////////////////////////////////////////////////////////////////////////////

analysis::KernelPartitioningPass::KernelPartitioningPass() {

}

analysis::KernelPartitioningPass::~KernelPartitioningPass() {
}

analysis::KernelPartitioningPass::KernelGraph *
 analysis::KernelPartitioningPass::runOnFunction(ir::PTXKernel &ptxKernel, SubkernelId baseId) {
	report("KernelPartitioningPass::runOnFunction(" << ptxKernel.name << ")");
	
	analysis::KernelPartitioningPass::BarrierPartitioning barrierPass;
	barrierPass.runOnKernel(ptxKernel);
	
	return new KernelGraph(&ptxKernel, baseId);
}


////////////////////////////////////////////////////////////////////////////////////////////////////

void analysis::KernelPartitioningPass::BarrierPartitioning::runOnKernel(ir::PTXKernel &ptxKernel) {
	report("partitioning blocks at barriers");

	ir::ControlFlowGraph *kernelCfg = ptxKernel.cfg();
	int barrierCount = 0;

	for (ir::ControlFlowGraph::iterator bb_it = kernelCfg->begin(); 
		bb_it != kernelCfg->end(); ++bb_it) {

		unsigned int n = 0;
		for (ir::BasicBlock::InstructionList::iterator inst_it = (bb_it)->instructions.begin();
			inst_it != (bb_it)->instructions.end(); ++inst_it, n++) {
			ir::PTXInstruction *inst = static_cast<ir::PTXInstruction *>(*inst_it);
			
			if (inst->opcode == ir::PTXInstruction::Ret) {
				inst->opcode = ir::PTXInstruction::Exit;
			}
			
			if (inst->opcode == ir::PTXInstruction::Bar) {
				report("  barrier in block " << bb_it->label << "(instruction " << n << ")");
				if (n + 1 < (unsigned int)(bb_it)->instructions.size()) {
				
					std::string label = (bb_it)->label + "_bar";
					ir::ControlFlowGraph::iterator block = kernelCfg->split_block(bb_it, n+1, 
						ir::BasicBlock::Edge::FallThrough, label);
					++barrierCount;
					break;
				}
			}
			else {
				// found a barrier that is the last instruction in a basic block - this could potentially
				// be along the subkernel frontier, so make sure this gets handled correctly.
			}
		}
	}
	report("  encountered " << barrierCount << " barriers");
}

////////////////////////////////////////////////////////////////////////////////////////////////////

analysis::KernelPartitioningPass::KernelGraph::KernelGraph(
	ir::PTXKernel *_kernel, 
	SubkernelId baseId)
: 
	ptxKernel(_kernel) 
{
	// data flow analysis
	_sourceKernelDfg = new analysis::DataflowGraph;
	_sourceKernelDfg->analyze(*ptxKernel);
	
	size_t spillRegionSize = _computeRegisterOffsets();
	
#if REPORT_BASE && REPORT_EMIT_SOURCE_PTXKERNEL
	report("Partitioning kernel " << _kernel->name);
	_kernel->write(std::cout);
#endif
	
	_createSpillRegion(spillRegionSize);
	_partition(baseId);
	_linkExternalEdges();
	_createHandlers();
}

analysis::KernelPartitioningPass::KernelGraph::~KernelGraph() {
	if (_sourceKernelDfg) {
		delete _sourceKernelDfg;
	}
	subkernels.clear();
}


void analysis::KernelPartitioningPass::KernelGraph::_partitionMaximumSize(SubkernelId baseId) {

	report("KernelGraph::_partitionMaximumSize()");
	
	Subkernel subkernel(baseId);
	entrySubkernelId = baseId;
	
	// add all blocks to subkernel
	ir::ControlFlowGraph *cfg = ptxKernel->cfg();
	for (ir::ControlFlowGraph::iterator bb_it = cfg->begin(); bb_it != cfg->end(); ++bb_it) {
		subkernel.sourceBlocks.insert(bb_it);
	}
	
	subkernel.create(ptxKernel, _sourceKernelDfg, registerOffsets);
	subkernels.insert(std::make_pair(subkernel.id, subkernel));
}

void analysis::KernelPartitioningPass::KernelGraph::_partitionMinimumSize(SubkernelId baseId) {

	report("KernelGraph::_partitionMinimumSize()");
	
	// add all blocks to subkernel
	ir::ControlFlowGraph *cfg = ptxKernel->cfg();
	for (ir::ControlFlowGraph::iterator bb_it = cfg->begin(); bb_it != cfg->end(); ++bb_it) {
		if (!bb_it->instructions.size()) {
			continue;
		}
		
		Subkernel subkernel(baseId + subkernels.size());
		subkernel.sourceBlocks.insert(bb_it);
		subkernel.create(ptxKernel, _sourceKernelDfg, registerOffsets);
		subkernels.insert(std::make_pair(subkernel.id, subkernel));
		
		if (subkernels.size() == 1) {
			entrySubkernelId = subkernel.id;
		}
	}
}

void analysis::KernelPartitioningPass::KernelGraph::_linkExternalEdges() {

	report("Linking external edges");
	
	for (SubkernelMap::iterator subkernel_it = subkernels.begin(); subkernel_it != subkernels.end(); 
		++subkernel_it) {
		
		Subkernel &subkernel = subkernel_it->second;
		for (ExternalEdgeVector::iterator edge_it = subkernel.outEdges.begin(); 
			edge_it != subkernel.outEdges.end(); ++edge_it) {
			
			bool found = false;
			for (SubkernelMap::iterator checksk_it = subkernels.begin(); 
				checksk_it != subkernels.end() && !found;
				++checksk_it) {
				
				Subkernel &checksk = checksk_it->second;
				if (checksk.id != subkernel.id) {
					for (ExternalEdgeVector::iterator inedge_it = checksk.inEdges.begin(); 
						inedge_it != checksk.inEdges.end() && !found; ++inedge_it) {
					
						ExternalEdge &inEdge = *inedge_it;
						if (inEdge.sourceEdge.head == edge_it->sourceEdge.head && 
							inEdge.sourceEdge.tail == edge_it->sourceEdge.tail && 
							inEdge.sourceEdge.type == edge_it->sourceEdge.type) {
						
							edge_it->entryId = inEdge.entryId;
							found = true;
							report("  linking " << edge_it->handler->label << " to " << inEdge.handler->label 
								<< " (entry " << edge_it->entryId << ")");
						}
					}	
				}
			}
			if (!found) {
				report(" failed to link external edge: " << edge_it->sourceEdge.head->label << " -> " 
					<< edge_it->sourceEdge.tail->label);
				edge_it->entryId = 0;
				edge_it->exitStatus = Thread_exit;
			}
		}
	}
}

/*!
	\brief constructs a partitioning of the PTX kernel according to some heuristic
		then uses these to create subkernels
*/
void analysis::KernelPartitioningPass::KernelGraph::_partition(SubkernelId baseId) {
	//
	// select partitioning heuristic here
	//
	// A partitioning constructs a set of basic-block sets. The edges are then
	// classified as internal if they do not cross partitions and external if they do.
	// External edges are further classified as in-edges or out-edges from the perspective
	// of each subkernel.
	//
	
	// construct subkernels according to one of several partitioning heuristics
	_partitionMinimumSize(baseId);
}

/*!
	\brief inserts local variable declarations for spill regions, resume points, and resume status
*/
void analysis::KernelPartitioningPass::KernelGraph::_createSpillRegion(size_t spillSize) {
	
	ir::PTXStatement resumeTarget(ir::PTXStatement::Local);
		
	resumeTarget.type = ir::PTXOperand::u32;
	resumeTarget.name = "_Zocelot_resume_point";
	
	ptxKernel->locals.insert(std::make_pair(resumeTarget.name, ir::Local(resumeTarget)));
	
	ir::PTXStatement resumeStatus(ir::PTXStatement::Local);
	resumeStatus.type = ir::PTXOperand::u32;
	resumeStatus.name = "_Zocelot_resume_status";
	ptxKernel->locals.insert(std::make_pair(resumeStatus.name, ir::Local(resumeStatus)));
	
	ir::PTXStatement spillRegion(ir::PTXStatement::Local);
	spillRegion.type = ir::PTXOperand::b8;
	spillRegion.name = "_Zocelot_spill_area";
	spillRegion.array.stride.push_back((unsigned int)spillSize);
	
	ptxKernel->locals.insert(std::make_pair(spillRegion.name, ir::Local(spillRegion)));
		
	report("  Spill region size is " << spillSize);
}


/*!

*/
void analysis::KernelPartitioningPass::KernelGraph::_createHandlers() {
	for (SubkernelMap::iterator subkernel_it = subkernels.begin(); subkernel_it != subkernels.end();
		++subkernel_it) {
		Subkernel &subkernel = subkernel_it->second;
		subkernel.createHandlers(_sourceKernelDfg, registerOffsets);
	}
}

analysis::KernelPartitioningPass::SubkernelId 
	analysis::KernelPartitioningPass::KernelGraph::getEntrySubkernel() const {
	
	return entrySubkernelId;
}

/*!
	\brief compute basic mapping
*/
size_t analysis::KernelPartitioningPass::KernelGraph::_computeRegisterOffsets() {
	typedef analysis::DataflowGraph::RegisterId RegisterId;
	
	size_t bytes = 0;
	RegisterId maxRegister = _sourceKernelDfg->maxRegister();
	for (RegisterId id = 0; id <= maxRegister; id++) {
		size_t offset = sizeof(int*) * id;
		registerOffsets[id] = offset;
		bytes = offset;
	}
	return bytes;
}

size_t analysis::KernelPartitioningPass::KernelGraph::localMemorySize() const {
	SubkernelMap::const_iterator subkernel_it = subkernels.find(entrySubkernelId);

	size_t localsSize = 0;
	for (ir::Kernel::LocalMap::const_iterator local_it = subkernel_it->second.subkernel->locals.begin();
		local_it != subkernel_it->second.subkernel->locals.end(); ++local_it ) {
	
		localsSize += local_it->second.getSize();
	}
	return localsSize;
}
			
////////////////////////////////////////////////////////////////////////////////////////////////////			
analysis::KernelPartitioningPass::Subkernel::Subkernel(SubkernelId _id): id(_id) {

}

analysis::KernelPartitioningPass::Subkernel::Subkernel() {
}

void analysis::KernelPartitioningPass::Subkernel::create(ir::PTXKernel *source,
	analysis::DataflowGraph *sourceDfg,
	const RegisterOffsetMap &registerOffsets) {

	report("Subkernel::create(" << source->name << ")");
	
	_create(source);
	
	
	subkernel->write(std::cout);
}

void analysis::KernelPartitioningPass::Subkernel::createHandlers(
	analysis::DataflowGraph *sourceDfg,
	const RegisterOffsetMap &registerOffsets) {
	
	analysis::DataflowGraph subkernelDfg;
	subkernelDfg.analyze(*subkernel);
	
	_createExternalHandlers(sourceDfg, &subkernelDfg, registerOffsets);
	_createBarrierHandlers(sourceDfg, &subkernelDfg, registerOffsets);
	_createDivergenceHandlers(sourceDfg, &subkernelDfg, registerOffsets);
	
	#if REPORT_BASE && REPORT_EMIT_SUBKERNEL_PTX
	subkernel->write(std::cout);
	#endif
}


void analysis::KernelPartitioningPass::Subkernel::_create(ir::PTXKernel *source) {

	report("Subkernel::_create(" << source->name << ")");
	
	std::stringstream ss;
	ss << "_subkernel_" << source->name << "_" << id;
	
	subkernel = new ir::PTXKernel(ss.str(), false, source->module);
	for (ir::Kernel::ParameterVector::const_iterator arg_it = source->arguments.begin();
		arg_it != source->arguments.end(); ++arg_it) {
		
		subkernel->arguments.push_back(*arg_it);
	}
	for (ir::Kernel::LocalMap::const_iterator local_it = source->locals.begin(); 
		local_it != source->locals.end(); ++local_it) {
		subkernel->locals.insert(std::make_pair(local_it->first, local_it->second));
	}
	
	std::vector< ir::BasicBlock::Edge > internalEdges;
	std::unordered_map< ir::BasicBlock::Pointer, ir::BasicBlock::Pointer> blockMapping;
	
	_analyzeExternalEdges(source, internalEdges, blockMapping);
	_analyzeBarriers(source, internalEdges, blockMapping);
	_analyzeDivergentControlFlow(source, internalEdges, blockMapping);
}

//! creates barrier entries and exits
void analysis::KernelPartitioningPass::Subkernel::_analyzeBarriers(
	ir::PTXKernel *source, EdgeVector &internalEdges, BasicBlockMap &blockMapping) {
	
	report(" _analyzeBarriers()");
	
	ir::ControlFlowGraph *subkernelCfg = subkernel->cfg();
	
	
	for (ir::ControlFlowGraph::iterator bb_it = subkernelCfg->begin(); 
		bb_it != subkernelCfg->end(); ++bb_it) {
		
		for (ir::BasicBlock::InstructionList::iterator inst_it = bb_it->instructions.begin();
			inst_it != bb_it->instructions.end(); ++inst_it) {
		
			const ir::PTXInstruction *inst = static_cast<const ir::PTXInstruction *>(*inst_it);
			
			if (inst->opcode == ir::PTXInstruction::Bar) {
				report("  barrier in block " << bb_it->label);
				
				ir::ControlFlowGraph::iterator blockBeforeBarrier = bb_it;
				ir::ControlFlowGraph::edge_iterator barrierEdge = bb_it->out_edges.front();
				ir::ControlFlowGraph::iterator blockAfterBarrier = barrierEdge->tail;
				
				// remove the split edge, add a new one between the barrier block and the exit
				subkernelCfg->remove_edge(barrierEdge);
				subkernelCfg->insert_edge(ir::BasicBlock::Edge(blockBeforeBarrier, 
					subkernelCfg->get_exit_block(), ir::BasicBlock::Edge::Dummy));

				// create a handler block and transform the source code
				ir::BasicBlock handlerBlock(blockAfterBarrier->label + "_barrier_entry");
				ir::ControlFlowGraph::iterator entryHandlerBlock = subkernelCfg->insert_block(handlerBlock);
				
				ir::ControlFlowGraph::edge_iterator entryEdge = subkernelCfg->insert_edge(ir::BasicBlock::Edge(
					entryHandlerBlock, blockAfterBarrier, ir::BasicBlock::Edge::Branch));
					
				ir::ControlFlowGraph::edge_iterator dummyEntry = subkernelCfg->insert_edge(ir::BasicBlock::Edge(
					subkernelCfg->get_entry_block(), entryHandlerBlock, ir::BasicBlock::Edge::Dummy));
									
				// construct a subkernel entry
				SubkernelId entryId = ExternalEdge::getEncodedEntry(id, 
					(SubkernelId)(inEdges.size() + barrierEntries.size()));
					
				ExternalEdge externalEntryEdge;
				externalEntryEdge.handler = entryHandlerBlock;
				externalEntryEdge.frontierBlock = blockAfterBarrier;
				externalEntryEdge.exitStatus = Thread_barrier;
				externalEntryEdge.entryId = entryId;
				barrierEntries.push_back(externalEntryEdge);

				ExternalEdge externalExitEdge;
				externalExitEdge.handler = blockBeforeBarrier;
				externalExitEdge.frontierBlock = blockBeforeBarrier;
				externalExitEdge.exitStatus = Thread_barrier;
				externalEntryEdge.entryId = entryId;
				barrierExits.push_back(externalExitEdge);
			}
		}
	}
}

//! creates external edges for subkernel entries and exits
void analysis::KernelPartitioningPass::Subkernel::_analyzeExternalEdges(
	ir::PTXKernel *source, EdgeVector &internalEdges, BasicBlockMap &blockMapping) {
	
	report(" _analyzeExternalEdges()");
	
	ir::ControlFlowGraph *subkernelCfg = subkernel->cfg();
	
	for (BasicBlockSet::iterator bb_it = sourceBlocks.begin();
		bb_it != sourceBlocks.end(); ++bb_it) {
		
		ir::BasicBlock newBlock((*bb_it)->label, (*bb_it)->id, (*bb_it)->instructions, 
			(*bb_it)->comment );
		
		report(" adding block " << newBlock.label);
		
		blockMapping[*bb_it] = subkernelCfg->insert_block(newBlock);
	}
	
	for (BasicBlockSet::iterator bb_it = sourceBlocks.begin();
		bb_it != sourceBlocks.end(); ++bb_it) {
		
		for (ir::BasicBlock::EdgePointerVector::iterator edge_it = (*bb_it)->out_edges.begin();
			edge_it != (*bb_it)->out_edges.end(); ++edge_it ) {
		
			//bool isExitEdge = (*edge_it)->tail == source->cfg()->get_exit_block();
			if (sourceBlocks.find((*edge_it)->tail) == sourceBlocks.end()) {
				
				ir::BasicBlock handler;
				std::string suffix = ((*edge_it)->tail->label != "" ? "_to_" : "");
				handler.label = (*edge_it)->head->label + "_external_out_handler" + suffix + 
					(*edge_it)->tail->label.substr(4);
				ir::ControlFlowGraph::iterator handlerBlock = subkernelCfg->insert_block(handler);
				
				outEdges.push_back(ExternalEdge(**edge_it, handlerBlock));
				
				report("  adding EXTERNAL OUT-Edge " << (*edge_it)->head->label << " -> " 
					<< (*edge_it)->tail->label);
			}
			else {
				report("  replicating internal edge " << (*edge_it)->head->label << " -> " 
					<< (*edge_it)->tail->label);
				internalEdges.push_back(**edge_it);
			}
		}
		
		for (ir::BasicBlock::EdgePointerVector::iterator edge_it = (*bb_it)->in_edges.begin();
			edge_it != (*bb_it)->in_edges.end(); ++edge_it) {
			
			bool isEntryEdge = (*edge_it)->head == source->cfg()->get_entry_block();
			if (sourceBlocks.find((*edge_it)->head) == sourceBlocks.end() && !isEntryEdge) {
			
				ir::BasicBlock handler;
				std::string suffix = ((*edge_it)->head->label != "" ? "_from_" : "");
				handler.label = (*edge_it)->tail->label + "_external_in_handler_" + suffix +
					(*edge_it)->head->label.substr(4);
				ir::ControlFlowGraph::iterator handlerBlock = subkernelCfg->insert_block(handler);
				
				// assign unique entryId 
				SubkernelId entryId = ExternalEdge::getEncodedEntry(id, (SubkernelId)inEdges.size());
				inEdges.push_back(ExternalEdge(**edge_it, handlerBlock, entryId));
				
				report("  adding EXTERNAL IN-Edge " << (*edge_it)->head->label << " -> " 
					<< (*edge_it)->tail->label);
			}
			if (isEntryEdge) {
				report(" ENTRY EDGE");
				internalEdges.push_back(**edge_it);
			}
		}
	}
	
	blockMapping[source->cfg()->get_entry_block()] = subkernelCfg->get_entry_block();
	blockMapping[source->cfg()->get_exit_block()] = subkernelCfg->get_exit_block();
	
	// create internal edges
	report(" creating internal edges");
	for (std::vector< ir::BasicBlock::Edge >::iterator edge_it = internalEdges.begin();
		edge_it != internalEdges.end(); ++edge_it) {
		
		report(" looking in block mapping: " << edge_it->head->label << " -> " << edge_it->tail->label);
		
		if (blockMapping.find(edge_it->head) == blockMapping.end()) {
			assert(0 && "Failed to find predecessor block in mapping");
		}
		if (blockMapping.find(edge_it->tail) == blockMapping.end()) {
			assert(0 && "Failed to find successor block in mapping");
		}
		
		ir::BasicBlock::Edge internalEdge(blockMapping[edge_it->head], 
			blockMapping[edge_it->tail], edge_it->type);
			
		report("  adding internal edge: " << internalEdge.head->label << " -> " << internalEdge.tail -> label);
		subkernelCfg->insert_edge(internalEdge);
	}
	
	// identify frontier blocks along in eges
	report(" identifying targets of external IN edges");
	for (ExternalEdgeVector::iterator edge_it = inEdges.begin();
		edge_it != inEdges.end(); ++edge_it) {
		
		edge_it->frontierBlock = blockMapping[edge_it->sourceEdge.tail];
		
		ir::BasicBlock::Edge handlerEdge(edge_it->handler, edge_it->frontierBlock, 
			ir::BasicBlock::Edge::Branch);
		subkernelCfg->insert_edge(handlerEdge);
	}
	
	// identify frontier blocks along out eges
	report(" identifying sources of external OUT edges");
	for (ExternalEdgeVector::iterator edge_it = outEdges.begin();
		edge_it != outEdges.end(); ++edge_it) {
		
		edge_it->frontierBlock = blockMapping[edge_it->sourceEdge.head];
		
		ir::BasicBlock::Edge handlerEdge(edge_it->frontierBlock, edge_it->handler, 
			edge_it->sourceEdge.type);
		subkernelCfg->insert_edge(handlerEdge);
	}
}

//! \brief identifies divergent control flow and constructs unreachable handlers for entries and exits
void analysis::KernelPartitioningPass::Subkernel::_analyzeDivergentControlFlow(
	ir::PTXKernel *source, EdgeVector &internalEdges, BasicBlockMap &blockMapping) {
	report(" _analyzeDivergentControlFlow()");
}


/*!
	\brief partitions blocks in parent kernel such that bar.sync is last instruction
*/
void analysis::KernelPartitioningPass::Subkernel::_partitionBlocksAtBarrier() {
	report("partitioning blocks at barriers");
	int barrierCount = 0;
	
	ir::ControlFlowGraph *subkernelCfg = subkernel->cfg();
	
	for (ir::ControlFlowGraph::iterator bb_it = subkernelCfg->begin(); 
		bb_it != subkernelCfg->end(); ++bb_it) {

		unsigned int n = 0;
		for (ir::BasicBlock::InstructionList::iterator inst_it = (bb_it)->instructions.begin();
			inst_it != (bb_it)->instructions.end(); ++inst_it, n++) {

			ir::PTXInstruction *inst = static_cast<ir::PTXInstruction *>(*inst_it);
			
			if (inst->opcode == ir::PTXInstruction::Ret) {
				inst->opcode = ir::PTXInstruction::Exit;
			}
			
			if (inst->opcode == ir::PTXInstruction::Bar) {
				report("  barrier in block " << bb_it->label << "(instruction " << n << ")");
				
				if (n + 1 < (unsigned int)(bb_it)->instructions.size()) {
				
					std::string label = (bb_it)->label + "_bar";
					ir::ControlFlowGraph::iterator block = subkernelCfg->split_block(bb_it, n+1, 
						ir::BasicBlock::Edge::FallThrough, label);
					
					ir::ControlFlowGraph::edge_iterator splitEdge = block->in_edges.front();
					ir::ControlFlowGraph::iterator beforeBarrierBlock = splitEdge->head;
					
					// remove the split edge, add a new one between the barrier block and the exit
					subkernelCfg->remove_edge(splitEdge);
					subkernelCfg->insert_edge(ir::BasicBlock::Edge(beforeBarrierBlock, 
						subkernelCfg->get_exit_block(), ir::BasicBlock::Edge::Dummy));

					// create a handler block and transform the source code
					ir::BasicBlock handlerBlock(block->label + "_barrier_entry");
					ir::ControlFlowGraph::iterator entryHandlerBlock = subkernelCfg->insert_block(handlerBlock);
					ir::ControlFlowGraph::edge_iterator entryEdge = subkernelCfg->insert_edge(ir::BasicBlock::Edge(
						entryHandlerBlock, block, ir::BasicBlock::Edge::Branch));
					ir::ControlFlowGraph::edge_iterator dummyEntry = subkernelCfg->insert_edge(ir::BasicBlock::Edge(
						subkernelCfg->get_entry_block(), entryHandlerBlock, ir::BasicBlock::Edge::Dummy));
										
					// construct a subkernel entry
					SubkernelId entryId = ExternalEdge::getEncodedEntry(id, 
						(SubkernelId)(inEdges.size() + barrierEntries.size()));
						
					ExternalEdge externalEntryEdge;
					externalEntryEdge.handler = entryHandlerBlock;
					externalEntryEdge.frontierBlock = block;
					externalEntryEdge.exitStatus = Thread_barrier;
					externalEntryEdge.entryId = entryId;
					barrierEntries.push_back(externalEntryEdge);

					ExternalEdge externalExitEdge;
					externalExitEdge.handler = beforeBarrierBlock;
					externalExitEdge.frontierBlock = beforeBarrierBlock;
					externalExitEdge.exitStatus = Thread_barrier;
					externalEntryEdge.entryId = entryId;
					barrierExits.push_back(externalExitEdge);
				
					report("  - partitioned into additional block: " << block->label);
				}
				
				++barrierCount;
				break;
			}
			else {
				// found a barrier that is the last instruction in a basic block - this could potentially
				// be along the subkernel frontier, so make sure this gets handled correctly.
			}
		}
	}
	
	report("  encountered " << barrierCount << " barriers");
}


class ExternalEdgeIterator {
public:
	typedef analysis::KernelPartitioningPass::ExternalEdgeVector ExternalEdgeVector;
	typedef analysis::KernelPartitioningPass::Subkernel Subkernel;
	typedef analysis::KernelPartitioningPass::ExternalEdge ExternalEdge;
	
	ExternalEdgeIterator(): subkernel(0), selector(0) { }
	
	ExternalEdgeIterator(
		Subkernel *_sk, 
		size_t _sel,
		 ExternalEdgeVector::iterator _it):
		 subkernel(_sk), selector(_sel), iterator(_it) {
		 
	}

	static ExternalEdgeIterator begin(Subkernel *_sk) { 

		return ExternalEdgeIterator(_sk, 0, _sk->outEdges.begin());
	}
	
	static ExternalEdgeIterator end(Subkernel *_sk) {
		return ExternalEdgeIterator(_sk, 5, _sk->divergentExits.end());
	}
	
	ExternalEdgeIterator &operator++() {
		static ExternalEdgeVector Subkernel::* member[] = {
			&Subkernel::outEdges, 
			&Subkernel::inEdges, 
			&Subkernel::barrierEntries, 
			&Subkernel::barrierExits, 
			&Subkernel::divergentEntries, 
			&Subkernel::divergentExits
		};
		if (++iterator == (subkernel->*(member[selector])).end()) {
			++selector;
		}
		return *this;
	}
	
	ExternalEdgeIterator operator++(int) {
		ExternalEdgeIterator copy(*this);
		(*this)++;
		return copy;
	}
	
	ExternalEdge &operator->() {
		return *iterator;
	}
	
	bool operator ==(const ExternalEdgeIterator &it) const {
		return (it.subkernel == subkernel && it.selector == selector && it.iterator == iterator);
	}
	
	bool operator !=(const ExternalEdgeIterator &it) const {
		return !(*this == it);
	}
	
	size_t index() const {
		return selector;
	}
	
protected:
	Subkernel *subkernel;

	size_t selector;
	
	ExternalEdgeVector::iterator iterator;
};

void analysis::KernelPartitioningPass::Subkernel::_determineRegisterUses(
	analysis::DataflowGraph::RegisterSet &uses) {

	ir::ControlFlowGraph *cfg = subkernel->cfg();
	std::unordered_set< ir::BasicBlock::Pointer > handlerBlocks;
	
	for (ExternalEdgeVector::iterator edge_it = inEdges.begin(); edge_it != inEdges.end(); ++edge_it) {
		handlerBlocks.insert(edge_it->handler);
	}
	for (ExternalEdgeVector::iterator edge_it = outEdges.begin(); edge_it != outEdges.end(); ++edge_it) {
		handlerBlocks.insert(edge_it->handler);
	}
	for (ir::ControlFlowGraph::iterator block_it = cfg->begin(); block_it != cfg->end(); ++block_it) {
		if (handlerBlocks.find(block_it) != handlerBlocks.end()) {
			continue;
		}
		for (ir::BasicBlock::InstructionList::iterator inst_it = block_it->instructions.begin();
			inst_it != block_it->instructions.end(); ++inst_it) {
			ir::PTXInstruction *instr = static_cast<ir::PTXInstruction*>(*inst_it);
			
			ir::PTXOperand ir::PTXInstruction::*operands[] = {
				&ir::PTXInstruction::pg,
				&ir::PTXInstruction::pq,
				&ir::PTXInstruction::d,
				&ir::PTXInstruction::a,
				&ir::PTXInstruction::b,
				&ir::PTXInstruction::c
			};
			for (int i = 0; i < 6; i++) {
				ir::PTXOperand &operand = (instr->*operands[i]);
				if (operand.addressMode == ir::PTXOperand::Register) {
					uses.insert(operand.reg);
				}
			}
		}
	}	
}

/*!
	create a handler block for each in edge that restores values
*/
void analysis::KernelPartitioningPass::Subkernel::_createExternalHandlers(
	analysis::DataflowGraph *sourceDfg,
	analysis::DataflowGraph *subkernelDfg,
	const RegisterOffsetMap &registerOffsets) {
	
	assert(sourceDfg && subkernelDfg);
	
	report("Subkernel::_createExternalHandlers()");
	
	analysis::DataflowGraph::IteratorMap cfgToDfg = sourceDfg->getCFGtoDFGMap();
	analysis::DataflowGraph::IteratorMap subkernelCfgToDfg = subkernelDfg->getCFGtoDFGMap();
	
	analysis::DataflowGraph::RegisterSet usedRegisters;
	_determineRegisterUses(usedRegisters);
	
	report("  visiting external IN-edges");
	for (ExternalEdgeVector::iterator edge_it = inEdges.begin();
		edge_it != inEdges.end(); ++edge_it) {
		
		assert(subkernelCfgToDfg.find(edge_it->handler) != subkernelCfgToDfg.end());
		
		// restore live values
		RegisterSet aliveValues = cfgToDfg[edge_it->sourceEdge.head]->aliveOut();
		auto handlerDfgBlock = subkernelCfgToDfg[edge_it->handler];
		
		report("    IN-edge: " << edge_it->handler->label << " -> " << edge_it->frontierBlock->label 
			<< " (" << aliveValues.size() << " live values");
		
		edge_it->handler->comment = boost::lexical_cast<std::string>(aliveValues.size()) 
			+ " live-in values";

		_spillLiveValues(subkernelDfg, handlerDfgBlock, usedRegisters, aliveValues, registerOffsets, true);
		
		ir::PTXInstruction bra(ir::PTXInstruction::Bra);
		bra.d = ir::PTXOperand(ir::PTXOperand::Label, edge_it->frontierBlock->label);
		subkernelDfg->insert(handlerDfgBlock, bra);
	}
	
	std::unordered_map< ir::BasicBlock::Pointer, std::vector<ExternalEdge> > frontierExitBlocks;
	
	// create a handler block for each out-edge that stores values
	report("  visiting external OUT-edges");
	for (ExternalEdgeVector::iterator edge_it = outEdges.begin();
		edge_it != outEdges.end(); ++edge_it) {
		
		assert(subkernelCfgToDfg.find(edge_it->handler) != subkernelCfgToDfg.end());
		
		// restore live values
		RegisterSet aliveValues = cfgToDfg[edge_it->sourceEdge.head]->aliveOut();
		auto handlerDfgBlock = subkernelCfgToDfg[edge_it->handler];
		auto frontierDfgBlock = cfgToDfg[edge_it->frontierBlock];
		
		report("    OUT-edge: " << edge_it->frontierBlock->label << " -> " << edge_it->handler->label
			<< " (" << aliveValues.size() << " live values");
		
		edge_it->handler->comment = boost::lexical_cast<std::string>(aliveValues.size()) 
			+ " live-out values";
		
		_spillLiveValues(subkernelDfg, handlerDfgBlock, usedRegisters, aliveValues, registerOffsets, false);

		_createExit(handlerDfgBlock, subkernelDfg, edge_it->exitStatus, edge_it->entryId);
		
		report("  adding " << edge_it->frontierBlock->label << " to frontierExitBlocks");
		frontierExitBlocks[edge_it->frontierBlock].push_back(*edge_it);
	}
	
	report("Barrier exits");
	
	for (ExternalEdgeVector::iterator edge_it = barrierExits.begin();
		edge_it != barrierExits.end(); ++edge_it) {
	
		report("  barrier exit: " << edge_it->frontierBlock->label);

		assert(subkernelCfgToDfg.find(edge_it->frontierBlock) != subkernelCfgToDfg.end());
		auto frontierDfgBlock = subkernelCfgToDfg[edge_it->frontierBlock];
		
		RegisterSet aliveValues = frontierDfgBlock->aliveOut();
		report("    " << aliveValues.size() << " live values at barrier");

		// restore live values
				/*		
		RegisterSet aliveValues = cfgToDfg[edge_it->sourceBlock]->aliveOut();
		*/
	}

	_updateHandlerControlFlow(frontierExitBlocks, subkernelDfg);
}

void analysis::KernelPartitioningPass::Subkernel::_spillLiveValues(
	analysis::DataflowGraph *subkernelDfg, 
	analysis::DataflowGraph::iterator handlerDfgBlock, 
	const analysis::DataflowGraph::RegisterSet &usedRegisters,
	const RegisterSet &aliveValues,
	const RegisterOffsetMap &registerOffsets,
	bool loadLive) {
	
	ir::PTXInstruction move(ir::PTXInstruction::Mov);
	
	for (RegisterSet::const_iterator alive_it = aliveValues.begin();
		alive_it != aliveValues.end(); ++alive_it) {
	
		if (usedRegisters.find(*alive_it) == usedRegisters.end()) {
			continue;
		}

		report("      alive-out: " << alive_it->id << " [type: " 
			<< ir::PTXOperand::toString(alive_it->type) << "]");
		
		if (alive_it == aliveValues.begin()) {
			move.a = ir::PTXOperand(ir::PTXOperand::Address, ir::PTXOperand::u32, "_Zocelot_spill_area");
			move.d = ir::PTXOperand(ir::PTXOperand::Register, ir::PTXOperand::u32, subkernelDfg->newRegister());
			
			subkernelDfg->insert(handlerDfgBlock, move);
		}

		ir::PTXInstruction restore(ir::PTXInstruction::St);
		
		if (loadLive) {
			restore.opcode = ir::PTXInstruction::Ld;
			restore.type = alive_it->type;
			restore.addressSpace = ir::PTXInstruction::Local;
			restore.a = ir::PTXOperand(ir::PTXOperand::Indirect, ir::PTXOperand::u32, move.d.reg, 
				registerOffsets.find(alive_it->id)->second);
			restore.d = ir::PTXOperand(ir::PTXOperand::Register, alive_it->type, alive_it->id);
		}
		else {
			restore.opcode = ir::PTXInstruction::St;
			restore.type = alive_it->type;
			restore.addressSpace = ir::PTXInstruction::Local;
			restore.d = ir::PTXOperand(ir::PTXOperand::Indirect, ir::PTXOperand::u32, move.d.reg, 
				registerOffsets.find(alive_it->id)->second);
			restore.a = ir::PTXOperand(ir::PTXOperand::Register, alive_it->type, alive_it->id);
		}
		subkernelDfg->insert(handlerDfgBlock, restore);
	}
}

void analysis::KernelPartitioningPass::Subkernel::_updateHandlerControlFlow(
	ExternalEdgeMap &frontierExitBlocks, analysis::DataflowGraph *subkernelDfg) {

	report("Frontier exit blocks:");
	
	analysis::DataflowGraph::IteratorMap subkernelCfgToDfg = subkernelDfg->getCFGtoDFGMap();
	
	for (auto block_it = frontierExitBlocks.begin(); 
		block_it != frontierExitBlocks.end(); ++block_it) {	
	
		// update control flow instructions
		ir::PTXInstruction *terminator = static_cast<ir::PTXInstruction *>(
			block_it->first->instructions.back());
		
		report(" block " << block_it->first->label << " (" << block_it->second.size() 
			<< " external edges) terminator: " << terminator->toString());
		
		if (terminator->opcode == ir::PTXInstruction::Bra) {
			for (ExternalEdgeVector::iterator edge_it = block_it->second.begin(); 
				edge_it != block_it->second.end(); ++edge_it) {
				
				ExternalEdge &externalEdge = *edge_it;
				if (externalEdge.sourceEdge.type == ir::BasicBlock::Edge::Branch) {
					report(" 1 external edge, modifying branch target to point to handler");
					terminator->d = ir::PTXOperand(ir::PTXOperand::Label, edge_it->handler->label);
				}
			}
		}
		else if (terminator->opcode == ir::PTXInstruction::Call) {
			assert(0 && "unhandled");
		}
		else if (terminator->opcode == ir::PTXInstruction::Bar) {
			report(" I forget what I wanted here. there are this many edges: " << block_it->second.size());
			report("    block: " << block_it->first->label);

			//block_it->first->instructions.pop_back();
			auto dfgBlock = subkernelCfgToDfg[block_it->first];
			analysis::DataflowGraph::InstructionVector::iterator instr_it = dfgBlock->instructions().end();
			--instr_it;
			
			subkernelDfg->erase(dfgBlock, instr_it );
			_createExit(dfgBlock, subkernelDfg, Thread_barrier, 
				block_it->second.at(0).entryId);
		}
		else if (terminator->opcode == ir::PTXInstruction::Exit) {
			ExternalEdge &externalEdge = block_it->second.front();
			terminator->opcode = ir::PTXInstruction::Bra;
			terminator->d = ir::PTXOperand(ir::PTXOperand::Label, externalEdge.handler->label);
		}
		else if (terminator->opcode == ir::PTXInstruction::Ret) {
			assert(0 && "unhandled return instruction");
		}
		else {
			// fall-through
			report(" fall-through non-control-flow instruction to external edge: " 
				<< terminator->toString());
		}
	}	
	
	report("end frontier exit blocks:");		
}

/*!
	\brief inserts a scheduler block 
*/
void analysis::KernelPartitioningPass::Subkernel::_createScheduler() {
	//
	// insert a block into the entry of a subkernel
	//	
}


void analysis::KernelPartitioningPass::Subkernel::_createBarrierHandlers(
	analysis::DataflowGraph *sourceDfg,
	analysis::DataflowGraph *subkernelDfg,
	const RegisterOffsetMap &registerOffsets) {
	
	report("_createBarrierHandlers()");
	
	
}

void analysis::KernelPartitioningPass::Subkernel::_createDivergenceHandlers(
	analysis::DataflowGraph *sourceDfg,
	analysis::DataflowGraph *subkernelDfg,
	const RegisterOffsetMap &registerOffsets) {
	
	report("_createDivergenceHandlers()");
}

void analysis::KernelPartitioningPass::Subkernel::_createYieldHandlers(
	analysis::DataflowGraph *sourceDfg,
	analysis::DataflowGraph *subkernelDfg,
	const RegisterOffsetMap &registerOffsets) {
	
}

void analysis::KernelPartitioningPass::Subkernel::_createExit(analysis::DataflowGraph::iterator block, 
	analysis::DataflowGraph *subkernelDfg, ThreadExitType type, SubkernelId target) {
	
	report("  creating exit in block " << block->block()->label);
	
	ir::PTXInstruction move(ir::PTXInstruction::Mov);
	move.a = ir::PTXOperand(ir::PTXOperand::Address, ir::PTXOperand::u32, "_Zocelot_resume_status");
	move.d = ir::PTXOperand(ir::PTXOperand::Register, ir::PTXOperand::u32, subkernelDfg->newRegister());
	subkernelDfg->insert(block, move);
	
	ir::PTXInstruction store(ir::PTXInstruction::St);
	store.type = ir::PTXOperand::u32;
	store.addressSpace = ir::PTXInstruction::Local;
	store.d = ir::PTXOperand(ir::PTXOperand::Indirect, ir::PTXOperand::u32, move.d.reg, 0);
	store.a = ir::PTXOperand(type, ir::PTXOperand::u32);
	subkernelDfg->insert(block, store);
	
	move.a = ir::PTXOperand(ir::PTXOperand::Address, ir::PTXOperand::u32, "_Zocelot_resume_point");
	move.d = ir::PTXOperand(ir::PTXOperand::Register, ir::PTXOperand::u32, subkernelDfg->newRegister());
	subkernelDfg->insert(block, move);
	
	store.type = ir::PTXOperand::u32;
	store.addressSpace = ir::PTXInstruction::Local;
	store.d = ir::PTXOperand(ir::PTXOperand::Indirect, ir::PTXOperand::u32, move.d.reg, 0);
	store.a = ir::PTXOperand(target, ir::PTXOperand::u32);
	subkernelDfg->insert(block, store);
	
	ir::PTXInstruction exit(ir::PTXInstruction::Exit);
	subkernelDfg->insert(block, exit);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

