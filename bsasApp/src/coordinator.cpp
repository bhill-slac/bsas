
#include <epicsStdio.h>

#include <pv/reftrack.h>
#include <pv/standardField.h>

#include "coordinator.h"

namespace pvd = epics::pvData;

namespace {

pvd::StructureConstPtr type_signals(pvd::getFieldCreate()->createFieldBuilder()
                                    ->setId("epics:nt/NTScalar:1.0")
                                    ->addArray("value", pvd::pvString)
                                    ->add("alarm", pvd::getStandardField()->alarm())
                                    ->add("timeStamp", pvd::getStandardField()->timeStamp())
                                    ->createStructure());

pvd::StructureConstPtr type_status(pvd::getFieldCreate()->createFieldBuilder()
                                   ->setId("epics:nt/NTTable:1.0")
                                   ->addArray("labels", pvd::pvString)
                                   ->addNestedStructure("value")
                                       ->addArray("PV", pvd::pvString)
                                       ->addArray("connected", pvd::pvBoolean)
                                       ->addArray("nEvent", pvd::pvULong)
                                       ->addArray("nBytes", pvd::pvULong)
                                       ->addArray("nDiscon", pvd::pvULong)
                                       ->addArray("nError", pvd::pvULong)
                                       ->addArray("nOFlow", pvd::pvULong)
                                   ->endNested()
                                   ->add("alarm", pvd::getStandardField()->alarm())
                                   ->add("timeStamp", pvd::getStandardField()->timeStamp())
                                   ->createStructure());

} // namespace

size_t Coordinator::num_instances;

Coordinator::Coordinator(CAContext &ctxt, pvas::StaticProvider &provider, const std::string &prefix)
    :ctxt(ctxt)
    ,provider(provider)
    ,prefix(prefix)
    ,pv_signals(pvas::SharedPV::buildReadOnly())
    ,pv_status(pvas::SharedPV::buildReadOnly())
    ,handler(pvd::Thread::Config(this, &Coordinator::handle)
             .prio(epicsThreadPriorityLow)
             .autostart(false)
             <<"BSAS "<<prefix)
    ,signals_changed(true)
    ,running(true)
{
    REFTRACE_INCREMENT(num_instances);
    pv_signals->open(type_signals);

    root_status = pvd::getPVDataCreate()->createPVStructure(type_status);
    pvd::BitSet changed;
    {
        pvd::shared_vector<std::string> labels;
        labels.push_back("PV");
        labels.push_back("connected");
        labels.push_back("#Event");
        labels.push_back("#Bytes");
        labels.push_back("#Discon");
        labels.push_back("#Error");
        labels.push_back("#OFlow");

        pvd::PVStringArrayPtr flabel(root_status->getSubFieldT<pvd::PVStringArray>("labels"));
        flabel->replace(pvd::freeze(labels));
        changed.set(flabel->getFieldOffset());
    }
    pv_status->open(*root_status, changed);

    provider.add(prefix+"SIG", pv_signals);
    provider.add(prefix+"STS", pv_status);

    handler.start();
}

Coordinator::~Coordinator()
{
    REFTRACE_DECREMENT(num_instances);
    {
        Guard G(mutex);
        running = false;
    }
    wakeup.signal();
    handler.exitWait();

    table_receiver.reset();
    collector.reset(); // joins collector worker and cancels CA subscriptions
}

void Coordinator::handle()
{
    Guard G(mutex);

    bool expire = false;

    while(running) {
        bool changing = signals_changed;
        signals_changed = false;

        if(changing) {
            // handle change of PV list
            Collector::names_t temp(signals);

            UnGuard U(G);

            provider.remove(prefix+"TBL");

            table_receiver.reset();
            collector.reset();

            collector.reset(new Collector(ctxt, temp, epicsThreadPriorityMedium+5));
            table_receiver.reset(new PVAReceiver(*collector));

            provider.add(prefix+"TBL", table_receiver->pv);
            std::cerr<<"Add "<<prefix<<"TBL\n";

        }

        if(expire || changing) {
            // update status table

            Collector::names_t pvnames(signals);

            {
                UnGuard U(G);

                epicsTimeStamp now;
                epicsTimeGetCurrent(&now);

                pvd::shared_vector<pvd::boolean> conn(pvnames.size());
                pvd::shared_vector<pvd::uint64> events(pvnames.size()),
                                                bytes(pvnames.size()),
                                                discons(pvnames.size()),
                                                errors(pvnames.size()),
                                                oflows(pvnames.size());

                assert(pvnames.size()==collector->pvs.size());

                for(size_t i=0, N=collector->pvs.size(); i<N; i++) {
                    const Collector::PV& pv = collector->pvs[i];
                    if(!pv.sub) {
                        conn[i] = 0;

                    } else {
                        Subscription& sub = *pv.sub;

                        Guard G2(sub.mutex);

                        conn[i] = sub.connected;

                        events[i]	= sub.nUpdates		- sub.lUpdates;
                        bytes[i]	= sub.nUpdateBytes	- sub.lUpdateBytes;
                        discons[i]	= sub.nDisconnects	- sub.lDisconnects;
                        errors[i]	= sub.nErrors		- sub.lErrors;
                        oflows[i]	= sub.nOverflows	- sub.lOverflows;

                        sub.lUpdates	= sub.nUpdates;
                        sub.lUpdateBytes= sub.nUpdateBytes;
                        sub.lDisconnects= sub.nDisconnects;
                        sub.lErrors		= sub.nErrors;
                        sub.lOverflows	= sub.nOverflows;
                    }
                }

                pvd::BitSet changed;

                pvd::PVScalarArrayPtr farr;

                if(changing) {
                    farr = root_status->getSubFieldT<pvd::PVScalarArray>("value.PV");
                    farr->putFrom(pvnames);
                    changed.set(farr->getFieldOffset());
                }

                farr = root_status->getSubFieldT<pvd::PVScalarArray>("value.connected");
                farr->putFrom(pvd::freeze(conn));
                changed.set(farr->getFieldOffset());

                farr = root_status->getSubFieldT<pvd::PVScalarArray>("value.nEvent");
                farr->putFrom(pvd::freeze(events));
                changed.set(farr->getFieldOffset());

                farr = root_status->getSubFieldT<pvd::PVScalarArray>("value.nBytes");
                farr->putFrom(pvd::freeze(bytes));
                changed.set(farr->getFieldOffset());

                farr = root_status->getSubFieldT<pvd::PVScalarArray>("value.nDiscon");
                farr->putFrom(pvd::freeze(discons));
                changed.set(farr->getFieldOffset());

                farr = root_status->getSubFieldT<pvd::PVScalarArray>("value.nError");
                farr->putFrom(pvd::freeze(errors));
                changed.set(farr->getFieldOffset());

                farr = root_status->getSubFieldT<pvd::PVScalarArray>("value.nOFlow");
                farr->putFrom(pvd::freeze(oflows));
                changed.set(farr->getFieldOffset());

                pvd::PVScalarPtr fscale;
                fscale = root_status->getSubFieldT<pvd::PVScalar>("timeStamp.secondsPastEpoch");
                fscale->putFrom<pvd::uint32>(now.secPastEpoch+POSIX_TIME_AT_EPICS_EPOCH);
                changed.set(fscale->getFieldOffset());
                fscale = root_status->getSubFieldT<pvd::PVScalar>("timeStamp.nanoseconds");
                fscale->putFrom<pvd::uint32>(now.nsec);
                changed.set(fscale->getFieldOffset());

                pv_status->post(*root_status, changed);
            }

        }

        {
            UnGuard U(G);
            expire = !wakeup.wait(1.0);
        }
    }
}

void Coordinator::SignalsHandler::onPut(const pvas::SharedPV::shared_pointer& pv, pvas::Operation& op)
{
    pvd::PVStringArray::const_shared_pointer value(op.value().getSubFieldT<pvd::PVStringArray>("value"));

    if(!op.changed().get(value->getFieldOffset())) return; // ignore attempt to put something other than .value

    std::tr1::shared_ptr<Coordinator> self(coordinator.lock());
    if(self) {
        {
            Guard G(self->mutex);
            self->signals = value->view();
            self->signals_changed = true;
        }
        self->wakeup.signal();
    }

    pv->post(op.value(), op.changed());
    op.complete();
}
