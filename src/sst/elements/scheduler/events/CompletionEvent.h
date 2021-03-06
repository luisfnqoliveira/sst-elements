// Copyright 2009-2016 Sandia Corporation. Under the terms
// of Contract DE-AC04-94AL85000 with Sandia Corporation, the U.S.
// Government retains certain rights in this software.
// 
// Copyright (c) 2009-2016, Sandia Corporation
// All rights reserved.
// 
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.

/*
 * Class representing node finishing event
 */

#ifndef __COMPLETIONEVENT_H__
#define __COMPLETIONEVENT_H__

namespace SST {
    namespace Scheduler {

        class CompletionEvent : public SST::Event {
            public:

                CompletionEvent(int jobNum) : SST::Event() {
                    this -> jobNum = jobNum;
                }

                CompletionEvent* copy(){
                    CompletionEvent* tmp = new CompletionEvent(this -> jobNum);
                    return tmp;
                }

                int jobNum;

                NotSerializable(CompletionEvent)
        };
    }
}
#endif

