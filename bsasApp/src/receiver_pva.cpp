
#include <epicsMath.h>
#include <errlog.h>

#include <pv/reftrack.h>
#include <pv/standardField.h>

#include "receiver_pva.h"

#include <epicsExport.h>

namespace pvd = epics::pvData;

static int receiverPVADebug;

namespace {
// adjust name to be a valid field name.  [A-Za-z_][A-Za-z0-9_]*
void mangleName(std::string& name)
{
    if(name.empty())
        throw std::runtime_error("Empty PV name not allowed");

    for(size_t i=0, N=name.size(); i<N; i++) {
        char c = name[i];
        if(c>='A' && c<='Z') {}
        else if(c>='a' && c<='z') {}
        else if(i!=0 && c>='0' && c<='9') {}
        else if(c=='_') {}
        else {
            name[i] = '_';
        }
    }
}

template<typename T>
struct default_value { static inline T is() { return 0; } };
template<> struct default_value<float>  { static inline float is() { return epicsNAN; } };
template<> struct default_value<double>  { static inline double is() { return epicsNAN; } };
template<> struct default_value<std::string>  { static inline std::string is() { return ""; } };

// scalar types other than string
template<typename T>
struct NumericScalarCopier : public PVAReceiver::ColCopy
{
    typedef typename T::value_type value_type;
    typename T::shared_pointer field;

    NumericScalarCopier(PVAReceiver& receiver, size_t coln) :PVAReceiver::ColCopy(receiver)
    {
        field = receiver.root
                ->getSubFieldT<pvd::PVStructure>("value")
                ->getSubFieldT<T>(receiver.columns.at(coln).fname);
    }
    virtual ~NumericScalarCopier() {}

    virtual void copy(const PVAReceiver::slices_t &s, size_t coln)
    {
        pvd::shared_vector<value_type> scratch(s.size(), default_value<value_type>::is());
        PVAReceiver::Column& column = receiver.columns.at(coln);

        for(size_t r=0, R=s.size(); r<R; r++) {
            DBRValue cell(s[r].second.at(coln));

            if(!cell.valid() && column.last.valid()) {
                // back fill from previous
                cell = column.last;
            }

            if(!cell.valid() || cell->sevr > 3) {
                // disconnected
                column.last.swap(cell);
                continue;

            } else if(cell->count!=1 || cell->buffer.original_type()!=column.ftype) {
                column.ftype = cell->buffer.original_type();
                column.isarray = cell->count!=1;
                receiver.retype = true;
                column.last.reset();
                if(receiverPVADebug>1) {
                    errlogPrintf("%s triggers type change from scalar %d to %s %d\n",
                                 column.fname.c_str(), column.ftype,
                                 cell->count==1?"scalar":"array", cell->buffer.original_type());
                }
                return;
            }
            assert(column.ftype==(pvd::ScalarType)pvd::ScalarTypeID<value_type>::value);

            // could just alias cell->buffer.data()
            const pvd::shared_vector<const value_type>& elem(pvd::static_shared_vector_cast<const value_type>(cell->buffer));
            assert(elem.size()==1);

            scratch[r] = elem[0];

            // NO backfill!  Backfill obscures whether or not we missed an update!!!
            // column.last.swap(cell);
            column.last.reset();
        }

        field->replace(pvd::freeze(scratch));
        receiver.changed.set(field->getFieldOffset());
    }
};

struct NumericArrayCopier : public PVAReceiver::ColCopy
{
    pvd::PVUnionArrayPtr field;
    pvd::UnionConstPtr utype;
    pvd::ScalarArrayConstPtr arrtype;

    NumericArrayCopier(PVAReceiver& receiver, size_t coln) :PVAReceiver::ColCopy(receiver)
    {
        field = receiver.root
                ->getSubFieldT<pvd::PVStructure>("value")
                ->getSubFieldT<pvd::PVUnionArray>(receiver.columns.at(coln).fname);

        utype = std::tr1::static_pointer_cast<const pvd::UnionArray>(field->getArray())->getUnion();
        arrtype = utype->getField<pvd::ScalarArray>(0);
        if(!arrtype)
            throw std::logic_error("mis-matched UnionArray with retype");
    }
    virtual ~NumericArrayCopier() {}

    virtual void copy(const PVAReceiver::slices_t &s, size_t coln)
    {
        pvd::PVUnionArray::svector scratch(s.size()); // initialized with NULLs
        PVAReceiver::Column& column = receiver.columns.at(coln);

        pvd::PVDataCreatePtr create(pvd::getPVDataCreate());

        for(size_t r=0, R=s.size(); r<R; r++) {
            DBRValue cell(s[r].second.at(coln));

            if(!cell.valid() && column.last.valid()) {
                // back fill from previous
                cell = column.last;
            }

            if(!cell.valid() || cell->sevr > 3) {
                // disconnected
                column.last.swap(cell);
                continue;

            } else if(cell->buffer.original_type()!=column.ftype) {
                column.ftype = arrtype->getElementType();
                // always an array.  never switches (back) to scalar
                receiver.retype = true;
                column.last.reset();
                if(receiverPVADebug>1) {
                    errlogPrintf("%s triggers type change from array %d to array %d\n",
                                 column.fname.c_str(), column.ftype,
                                 cell->buffer.original_type());
                }
                return;
            }

            pvd::PVScalarArrayPtr arr(create->createPVScalarArray(arrtype));
            arr->putFrom(cell->buffer);

            pvd::PVUnionPtr U(create->createPVUnion(utype));
            U->set(0, arr);
            scratch[r] = U;

            column.last.swap(cell);
        }

        field->replace(pvd::freeze(scratch));
        receiver.changed.set(field->getFieldOffset());
    }
};

} // namespace

size_t PVAReceiver::num_instances;

PVAReceiver::PVAReceiver(Collector& collector)
    :collector(collector)
    ,pv(pvas::SharedPV::buildReadOnly())
    ,retype(true)
{
    REFTRACE_INCREMENT(num_instances);
    collector.add_receiver(this); // calls our names()
}

PVAReceiver::~PVAReceiver()
{
    REFTRACE_DECREMENT(num_instances);
    close();
}

void PVAReceiver::close()
{
    collector.remove_receiver(this);
    pv->close();
}

void PVAReceiver::names(const std::vector<std::string>& pvs)
{
    columns_t cols(pvs.size());
    pvd::shared_vector<std::string> Ls(pvs.size());

    for(size_t i=0, N=pvs.size(); i<N; i++) {
        Column& col = cols[i];
        col.fname = Ls[i] = pvs[i];
        mangleName(col.fname);

        // assume a signals are scalar double until proven false
        col.ftype = epics::pvData::pvDouble;
        col.isarray = false;

    }

    Ls.push_back("secondsPastEpoch");
    Ls.push_back("nanoseconds");

    {
        Guard G(mutex);
        columns.swap(cols);
        labels = pvd::freeze(Ls);

        root.reset();
        changed.clear();

        retype = true;
    }

    pv->close(); // paranoia?
}

void PVAReceiver::slices(const slices_t& s)
{
    {
        Guard G(mutex); // safely on Collector worker thread, but paranoia...

        if(retype) {
            retype = false;
            if(receiverPVADebug>0) {
                errlogPrintf("PVAReceiver type change\n");
            }

            pvd::FieldBuilderPtr builder(pvd::getFieldCreate()->createFieldBuilder()
                                         ->setId("epics:nt/NTTable:1.0")
                                         ->addArray("labels", pvd::pvString)
                                         ->addNestedStructure("value"));

            for(size_t i=0, N=columns.size(); i<N; i++) {
                Column& col = columns[i];
                if(!col.isarray) {
                    builder = builder->addArray(col.fname, col.ftype);
                } else {
                    builder = builder->addNestedUnionArray(col.fname)
                                        ->addArray("arr", col.ftype)
                                     ->endNested();
                }
            }

            pvd::StructureConstPtr type(builder
                                            ->addArray("secondsPastEpoch", pvd::pvUInt)
                                            ->addArray("nanoseconds", pvd::pvUInt)
                                        ->endNested() // end of .value
                                        //->add("alarm", pvd::getStandardField()->alarm())
                                        //->add("timeStamp", pvd::getStandardField()->timeStamp())
                                        ->createStructure());
            root = pvd::getPVDataCreate()->createPVStructure(type);
            changed.clear();

            fsec = root->getSubFieldT<pvd::PVUIntArray>("value.secondsPastEpoch");
            fnsec = root->getSubFieldT<pvd::PVUIntArray>("value.nanoseconds");

            {
                pvd::PVStringArrayPtr flabels(root->getSubFieldT<pvd::PVStringArray>("labels"));
                flabels->replace(labels);
                changed.set(flabels->getFieldOffset());
            }

            pvd::PVStructurePtr value(root->getSubFieldT<pvd::PVStructure>("value"));

            for(size_t c=0, C=columns.size(); c<C; c++) {
                Column& col = columns[c];

                if(!col.isarray && col.ftype==pvd::pvDouble) {
                    col.copier.reset(new NumericScalarCopier<pvd::PVDoubleArray>(*this, c));
                } else if(!col.isarray && col.ftype==pvd::pvInt) {
                    col.copier.reset(new NumericScalarCopier<pvd::PVIntArray>(*this, c));
                } else if(!col.isarray && col.ftype==pvd::pvUInt) {
                    col.copier.reset(new NumericScalarCopier<pvd::PVUIntArray>(*this, c));
                } else if(col.isarray) {
                    col.copier.reset(new NumericArrayCopier(*this, c));
                } else {
                    // TODO: not supported
                }
            }

            {
                UnGuard U(G);
                pv->close();
                pv->open(*root, changed);
            }
        } // end retype
        pvd::shared_vector<pvd::uint32> sec(s.size()), nsec(s.size());

        for(size_t r=0, R=s.size(); r<R; r++) {
            epicsUInt64 key = s[r].first;
            sec[r] = (key>>32) + POSIX_TIME_AT_EPICS_EPOCH;
            nsec[r] = key;
        }

        fsec->replace(pvd::freeze(sec));
        fnsec->replace(pvd::freeze(nsec));
        changed.set(fsec->getFieldOffset());
        changed.set(fnsec->getFieldOffset());

        for(size_t c=0, C=columns.size(); c<C; c++) { // for each column
            Column& col = columns[c];

            if(col.copier)
                col.copier->copy(s, c);
        }

        try {
            UnGuard U(G);
            pv->post(*root, changed);
        }catch(std::logic_error&) {
            // Assumed "Not open()".
            // Race between names() calling us and Collector worker calling us during startup.
            // Could avoid this with additional state tracking, but can simply ignore with no
            // ill effects.
        }

        changed.clear();
    }

}

extern "C" {
epicsExportAddress(int, receiverPVADebug);
}
