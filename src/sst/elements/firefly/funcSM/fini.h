// Copyright 2013-2016 Sandia Corporation. Under the terms
// of Contract DE-AC04-94AL85000 with Sandia Corporation, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2013-2016, Sandia Corporation
// All rights reserved.
//
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.

#ifndef COMPONENTS_FIREFLY_FUNCSM_FINI_H
#define COMPONENTS_FIREFLY_FUNCSM_FINI_H

#include "funcSM/barrier.h"

namespace SST {
namespace Firefly {

class FiniFuncSM :  public BarrierFuncSM 
{
  public:
    FiniFuncSM( SST::Params& params ) : BarrierFuncSM( params ) {}

    virtual void handleStartEvent( SST::Event* e, Retval& retval) {

        FiniStartEvent* event = static_cast<FiniStartEvent*>( e );
        BarrierStartEvent* tmp = new BarrierStartEvent( MP::GroupWorld  );

        delete event;

        BarrierFuncSM::handleStartEvent(static_cast<SST::Event*>(tmp), retval );
    }

    virtual void handleEnterEvent( Retval& retval ) {
        BarrierFuncSM::handleEnterEvent( retval );
    }

    virtual std::string protocolName() { return "CtrlMsgProtocol"; }
};

}
}

#endif
